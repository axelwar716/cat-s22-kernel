/* Copyright (c) 2018, 2020, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/leds.h>
#include <linux/pwm.h>
#include <linux/of_device.h>
#include <linux/uaccess.h>
#include <linux/dma-buf.h>

#include "mdss_panel.h"
#include "mdss_fb.h"
#include "mdss_spi_panel.h"
#include "s22_kclock.h"
#include "mdss_spi_client.h"
#include "mdss_mdp.h"
#include "mdss_sync.h"

enum {
	MDSS_SPI_RELEASE_FENCE = 0,
	MDSS_SPI_RETIRE_FENCE,
};

/*
 * mdss_spi_tx_pixel() sends every frame with one static spi_message, so a
 * frame may only go out once the previous one has completed.  Sending it
 * again while it is still queued re-inits a listed message and corrupts the
 * SPI queue: spi_sync callers (s22_kclock, panel commands) then never return
 * and take suspend and the compositor with them (04-10-2026, lid closed).
 * A busy kickoff waits up to S22_KOFF_DROP_MS (the SPI core fails a stuck
 * frame in ~1 s) and then drops the frame; the next one has the content.
 */
#define S22_KOFF_DROP_MS	1500
static unsigned int s22_frames_dropped;
module_param(s22_frames_dropped, uint, 0444);

static int mdss_spi_get_img(struct spi_panel_data *ctrl_pdata,
			struct mdp_layer_commit_v1 *commit, struct device *dev)
{
	struct msmfb_data image;
	struct dma_buf *dmabuf;
	void *vaddr;

	memset(&image, 0, sizeof(image));
	image.memory_id = commit->input_layers[0].buffer.planes[0].fd;
	image.offset = commit->input_layers[0].buffer.planes[0].offset;

	dmabuf = dma_buf_get(image.memory_id);
	if (IS_ERR(dmabuf)) {
		pr_err("%s : error on dma_buf_get\n", __func__);
		return PTR_ERR(dmabuf);
	}
	ctrl_pdata->image_data.srcp_attachment =
				dma_buf_attach(dmabuf, dev);
	if (IS_ERR(ctrl_pdata->image_data.srcp_attachment))
		goto err_put;

	ctrl_pdata->image_data.srcp_table =
		dma_buf_map_attachment(ctrl_pdata->image_data.srcp_attachment,
		DMA_TO_DEVICE);
	if (IS_ERR(ctrl_pdata->image_data.srcp_table))
		goto err_detach;

	dma_buf_begin_cpu_access(dmabuf, DMA_TO_DEVICE);

	vaddr  = dma_buf_kmap(dmabuf, 0);
	if (!vaddr) {
		pr_err("%s:ion memory mapping failed\n", __func__);
		goto err_unmap;
	};

	ctrl_pdata->image_data.addr = vaddr;
	ctrl_pdata->image_data.len = dmabuf->size;
	ctrl_pdata->image_data.mapped = true;
	ctrl_pdata->image_data.srcp_dma_buf = dmabuf;

	return 0;
err_unmap:
	dma_buf_unmap_attachment(ctrl_pdata->image_data.srcp_attachment,
		ctrl_pdata->image_data.srcp_table, DMA_BIDIRECTIONAL);
err_detach:
	dma_buf_detach(ctrl_pdata->image_data.srcp_dma_buf,
			ctrl_pdata->image_data.srcp_attachment);
err_put:
	dma_buf_put(ctrl_pdata->image_data.srcp_dma_buf);
	return -EINVAL;
}

static void mdss_spi_put_img(struct spi_panel_data *ctrl_pdata)
{
	if (!ctrl_pdata->image_data.mapped)
		return;
	dma_buf_kunmap(ctrl_pdata->image_data.srcp_dma_buf, 0,
				ctrl_pdata->image_data.addr);
	dma_buf_end_cpu_access(ctrl_pdata->image_data.srcp_dma_buf,
				DMA_BIDIRECTIONAL);
	dma_buf_unmap_attachment(ctrl_pdata->image_data.srcp_attachment,
			ctrl_pdata->image_data.srcp_table, DMA_TO_DEVICE);
	dma_buf_detach(ctrl_pdata->image_data.srcp_dma_buf,
				ctrl_pdata->image_data.srcp_attachment);
	dma_buf_put(ctrl_pdata->image_data.srcp_dma_buf);

	ctrl_pdata->image_data.srcp_dma_buf = NULL;
	ctrl_pdata->image_data.addr = NULL;
	ctrl_pdata->image_data.len = 0;
	ctrl_pdata->image_data.mapped = false;
}

static struct mdss_fence *__create_spi_display_fence(
	struct msm_fb_data_type *mfd,
	struct msm_sync_pt_data *sync_pt_data, u32 fence_type,
	int *fence_fd, int value)
{
	struct mdss_fence *sync_fence = NULL;
	char fence_name[32];

	if (fence_type == MDSS_SPI_RETIRE_FENCE) {
		scnprintf(fence_name, sizeof(fence_name), "fb%d_retire",
			mfd->index);
		sync_fence = mdss_fb_sync_get_fence(
			sync_pt_data->timeline_retire, fence_name, value);
	} else {
		scnprintf(fence_name, sizeof(fence_name), "fb%d_release",
			mfd->index);
		sync_fence = mdss_fb_sync_get_fence(
			sync_pt_data->timeline, fence_name, value);
	}

	if (IS_ERR_OR_NULL(sync_fence)) {
		pr_err("%s: unable to retrieve release fence\n", fence_name);
		goto end;
	}

	/* get fence fd */
	*fence_fd = mdss_get_sync_fence_fd(sync_fence);
	if (*fence_fd < 0) {
		pr_err("%s: get_unused_fd_flags failed error:0x%x\n",
			fence_name, *fence_fd);
		mdss_put_sync_fence(sync_fence);
		sync_fence = NULL;
		goto end;
	}
	pr_debug("%s:val=%d\n", mdss_get_sync_fence_name(sync_fence), value);

end:
	return sync_fence;
}

/*
 * __handle_buffer_fences() - copy sync fences and return release/retire
 * fence to caller.
 *
 * This function copies all input sync fences to acquire fence array and
 * returns release/retire fences to caller. It acts like buff_sync ioctl.
 */
static int __handle_buffer_fences(struct msm_fb_data_type *mfd,
	struct mdp_layer_commit_v1 *commit, struct mdp_input_layer *layer_list)
{
	struct mdss_fence *fence, *release_fence, *retire_fence;
	struct msm_sync_pt_data *sync_pt_data = NULL;
	struct mdp_input_layer *layer;
	int value;

	u32 acq_fen_count, i, ret = 0;
	u32 layer_count = commit->input_layer_cnt;

	sync_pt_data = &mfd->mdp_sync_pt_data;
	if (!sync_pt_data) {
		pr_err("sync point data are NULL\n");
		return -EINVAL;
	}

	i = mdss_fb_wait_for_fence(sync_pt_data);
	if (i > 0)
		pr_warn("%s: waited on %d active fences\n",
			sync_pt_data->fence_name, i);

	mutex_lock(&sync_pt_data->sync_mutex);
	for (i = 0, acq_fen_count = 0; i < layer_count; i++) {
		layer = &layer_list[i];

		if (layer->buffer.fence < 0)
			continue;

		fence = mdss_get_fd_sync_fence(layer->buffer.fence);
		if (!fence) {
			pr_err("%s: sync fence get failed! fd=%d\n",
				sync_pt_data->fence_name, layer->buffer.fence);
			ret = -EINVAL;
			break;
		}
		sync_pt_data->acq_fen[acq_fen_count++] = fence;
	}
	sync_pt_data->acq_fen_cnt = acq_fen_count;
	if (ret)
		goto sync_fence_err;

	value = sync_pt_data->threshold +
			atomic_read(&sync_pt_data->commit_cnt);

	release_fence = __create_spi_display_fence(mfd, sync_pt_data,
		MDSS_SPI_RELEASE_FENCE, &commit->release_fence, value);
	if (IS_ERR_OR_NULL(release_fence)) {
		pr_err("unable to retrieve release fence\n");
		ret = PTR_ERR(release_fence);
		goto release_fence_err;
	}

	retire_fence = __create_spi_display_fence(mfd, sync_pt_data,
		MDSS_SPI_RETIRE_FENCE, &commit->retire_fence, value);
	if (IS_ERR_OR_NULL(retire_fence)) {
		pr_err("unable to retrieve retire fence\n");
		ret = PTR_ERR(retire_fence);
		goto retire_fence_err;
	}

	mutex_unlock(&sync_pt_data->sync_mutex);
	return ret;

retire_fence_err:
	put_unused_fd(commit->release_fence);
	mdss_put_sync_fence(release_fence);
release_fence_err:
	commit->retire_fence = -1;
	commit->release_fence = -1;
sync_fence_err:
	for (i = 0; i < sync_pt_data->acq_fen_cnt; i++)
		mdss_put_sync_fence(sync_pt_data->acq_fen[i]);
	sync_pt_data->acq_fen_cnt = 0;

	mutex_unlock(&sync_pt_data->sync_mutex);

	return ret;
}

void mdss_spi_display_notifier_register(struct spi_panel_data *ctrl_pdata,
	struct notifier_block *notifier)
{
	blocking_notifier_chain_register(&ctrl_pdata->notifier_head, notifier);
}

void mdss_spi_display_notifier_unregister(struct spi_panel_data *ctrl_pdata,
	struct notifier_block *notifier)
{
	blocking_notifier_chain_unregister(&ctrl_pdata->notifier_head,
		notifier);
}

int mdss_spi_display_notify(struct spi_panel_data *ctrl_pdata, int event)
{
	return blocking_notifier_call_chain(&ctrl_pdata->notifier_head,
		event, ctrl_pdata);
}

int mdss_spi_display_pre_commit(struct msm_fb_data_type *mfd,
	struct file *file, struct mdp_layer_commit_v1 *commit)
{
	char *temp_buf;
	int rc = 0, scan_count = 0;
	int panel_yres, panel_xres;
	int padding_length, byte_per_pixel;
	int dma_stride, actual_stride;
	struct mdss_panel_data *pdata;
	struct spi_panel_data *ctrl_pdata = NULL;

	if (commit->input_layer_cnt == 0) {
		pr_err("SPI display doesn't support NULL commit\n");
		return 0;
	}

	pdata = dev_get_platdata(&mfd->pdev->dev);
	ctrl_pdata = container_of(pdata, struct spi_panel_data, panel_data);

	rc = mdss_spi_get_img(ctrl_pdata, commit, &mfd->pdev->dev);
	if (rc) {
		pr_err("mdss_spi_get_img failed\n");
		return rc;
	}

	panel_xres = ctrl_pdata->panel_data.panel_info.xres;
	panel_yres = ctrl_pdata->panel_data.panel_info.yres;
	dma_stride = mfd->fbi->fix.line_length;
	byte_per_pixel = ctrl_pdata->panel_data.panel_info.bpp / 8;
	actual_stride = panel_xres * byte_per_pixel;
	padding_length = dma_stride - actual_stride;

	/* remove padding and copy to continuous buffer */
	while (scan_count < panel_yres) {
		if (!(ctrl_pdata->back_buf) || !(ctrl_pdata->image_data.addr)) {
			pr_err("Null Pointer, return\n");
			return 0;
		}
		memcpy((ctrl_pdata->back_buf + (scan_count * actual_stride)),
			(ctrl_pdata->image_data.addr + (scan_count *
			(actual_stride + padding_length))), actual_stride);
				scan_count++;
	}

	mdss_spi_put_img(ctrl_pdata);
	s22_kclock_compose(ctrl_pdata, (u16 *)ctrl_pdata->back_buf);

	/* wait for SPI transfer done */
	rc = mdss_spi_wait_tx_done(ctrl_pdata);
	if (!rc) {
		pr_err("SPI transfer timeout\n");
		mdss_spi_display_notify(ctrl_pdata, MDP_NOTIFY_FRAME_TIMEOUT);
		return -EINVAL;
	}
	mdss_spi_display_notify(ctrl_pdata, MDP_NOTIFY_FRAME_DONE);

	/* swap buffer */
	temp_buf = ctrl_pdata->front_buf;
	ctrl_pdata->front_buf = ctrl_pdata->back_buf;
	ctrl_pdata->back_buf = temp_buf;
	ctrl_pdata->layer_commit_pending = true;
	__handle_buffer_fences(mfd, commit, commit->input_layers);

	return 0;
}

int mdss_spi_display_atomic_validate(struct msm_fb_data_type *mfd,
	struct file *file, struct mdp_layer_commit_v1 *commit)
{
	struct mdss_panel_data *pdata;

	pdata = dev_get_platdata(&mfd->pdev->dev);

	if ((commit->input_layers->dst_rect.w != pdata->panel_info.xres) &&
		(commit->input_layers->dst_rect.h != pdata->panel_info.yres) &&
		(commit->input_layer_cnt > 1)) {
		WARN_ONCE(1, "%s:Only support GPU composition layer_cnt %d\n",
				__func__, commit->input_layer_cnt);
		return -EINVAL;
	}

	if (commit->input_layers[0].buffer.format != MDP_RGB_565) {
		WARN_ONCE(1, "%s:SPI display only support RGB565 format %d\n",
			__func__, commit->input_layers[0].buffer.format);
		return -EINVAL;
	}

	return 0;
}

/*
 * CAT's vendor compositor drives the cover the way the stock OEM mdss_fb2
 * driver expected: it mmap()s the framebuffer, writes the GPU-composed
 * RGBA8888 frame into one of the two pages and pans with FBIOPUT_VSCREENINFO.
 * Convert that page into the panel's RGB565 back buffer, exactly like the
 * stock mdss_spi_panel_kickoff did (R from byte 0, G byte 1, B byte 2).
 */
static int mdss_spi_display_fb_to_back_buf(struct msm_fb_data_type *mfd,
	struct spi_panel_data *ctrl_pdata, u32 yoffset)
{
	struct fb_info *fbi = mfd->fbi;
	u32 xres = ctrl_pdata->panel_data.panel_info.xres;
	u32 yres = ctrl_pdata->panel_data.panel_info.yres;
	u32 stride = fbi->fix.line_length;
	u32 bpp = fbi->var.bits_per_pixel;
	const u8 *src;
	u16 *dst = (u16 *)ctrl_pdata->back_buf;
	u32 x, y;

	if (!fbi->screen_base || !dst)
		return -ENOMEM;
	if (bpp != 32 && bpp != 16)
		return -EINVAL;
	if (yoffset > fbi->var.yres_virtual - yres)
		return -EINVAL;
	if ((u64)(yoffset + yres) * stride > fbi->fix.smem_len)
		return -EINVAL;
	if (xres * (bpp / 8) > stride ||
		(u64)xres * yres * 2 > ctrl_pdata->byte_per_frame)
		return -EINVAL;

	src = (const u8 *)fbi->screen_base + yoffset * stride;
	for (y = 0; y < yres; y++) {
		const u8 *row = src + y * stride;

		if (bpp == 16) {
			memcpy(dst, row, xres * 2);
		} else {
			for (x = 0; x < xres; x++, row += 4)
				dst[x] = ((row[0] & 0xf8) << 8) |
					 ((row[1] & 0xfc) << 3) |
					 (row[2] >> 3);
		}
		dst += xres;
	}

	return 0;
}

int mdss_spi_display_kickoff(struct msm_fb_data_type *mfd,
			struct mdp_display_commit *data)
{
	struct spi_panel_data *ctrl_pdata = NULL;
	struct mdss_panel_data *pdata;
	bool busy = false;
	int rc = 0;

	pdata = dev_get_platdata(&mfd->pdev->dev);
	if (WARN_ON(!pdata))
		return -EINVAL;

	ctrl_pdata = container_of(pdata, struct spi_panel_data, panel_data);

	/* the previous async transfer still owns front_buf and the message */
	if (!ctrl_pdata->layer_commit_pending &&
	    !mdss_spi_wait_tx_done(ctrl_pdata) &&
	    !wait_event_timeout(ctrl_pdata->tx_done_waitq,
			atomic_read(&ctrl_pdata->koff_cnt) == 0,
			msecs_to_jiffies(S22_KOFF_DROP_MS)))
		busy = true;

	if (!ctrl_pdata->layer_commit_pending && !busy) {
		u32 yoffset = data ? data->var.yoffset : mfd->fbi->var.yoffset;
		char *temp_buf;

		rc = mdss_spi_display_fb_to_back_buf(mfd, ctrl_pdata, yoffset);
		if (rc) {
			pr_err_ratelimited("fb%d: no frame to send (%d)\n",
				mfd->index, rc);
			return rc;
		}
		s22_kclock_compose(ctrl_pdata, (u16 *)ctrl_pdata->back_buf);
		temp_buf = ctrl_pdata->front_buf;
		ctrl_pdata->front_buf = ctrl_pdata->back_buf;
		ctrl_pdata->back_buf = temp_buf;
	}
	ctrl_pdata->layer_commit_pending = false;

	mdss_spi_display_notify(ctrl_pdata, MDP_NOTIFY_FRAME_BEGIN);

	/*
	 * The kernel clock owns the cover (autonomous mode): the frame stays in
	 * front_buf (sent if the cover is handed back to Android) and nothing
	 * goes over SPI; the compositor sees a normal flushed frame.  The flag
	 * only changes under spi_tx_mutex, so check it there.
	 */
	if (s22_kclock_swallow(ctrl_pdata, false)) {
		bool swallow;

		mutex_lock(&ctrl_pdata->spi_tx_mutex);
		swallow = s22_kclock_swallow(ctrl_pdata, true);
		mutex_unlock(&ctrl_pdata->spi_tx_mutex);
		if (swallow) {
			mdss_spi_display_notify(ctrl_pdata,
				MDP_NOTIFY_FRAME_FLUSHED);
			return 0;
		}
	}

	enable_spi_panel_te_irq(ctrl_pdata, true);
	mutex_lock(&ctrl_pdata->spi_tx_mutex);
	if (s22_kclock_swallow(ctrl_pdata, true)) {
		rc = 0;
		goto flushed;
	}
	/* koff_cnt only rises here, under spi_tx_mutex: check and send */
	if (busy || atomic_read(&ctrl_pdata->koff_cnt)) {
		s22_frames_dropped++;
		pr_err_ratelimited("fb%d: SPI busy, frame dropped (%u)\n",
			mfd->index, s22_frames_dropped);
		rc = 0;
		goto flushed;
	}
	reinit_completion(&ctrl_pdata->spi_panel_te);
	atomic_inc(&ctrl_pdata->koff_cnt);

	/*
	 * The S22 cover's init sequence never enables TE (no 0x35 command),
	 * so the stock driver sent frames without waiting.  Only wait when
	 * the panel has actually pulsed TE at least once.
	 */
	if (ctrl_pdata->te_seen) {
		rc = wait_for_completion_timeout(&ctrl_pdata->spi_panel_te,
				msecs_to_jiffies(SPI_PANEL_TE_TIMEOUT));
		if (rc == 0)
			pr_err_ratelimited("wait panel TE time out, sending anyway\n");
	}
#ifndef TARGET_HW_MDSS_MDP3
	rc = mdss_spi_tx_pixel(ctrl_pdata->front_buf,
				ctrl_pdata->byte_per_frame,
				mdss_spi_tx_fb_complete, ctrl_pdata);
	/* not queued: no completion will drop the count */
	if (rc && atomic_add_unless(&ctrl_pdata->koff_cnt, -1, 0))
		wake_up_all(&ctrl_pdata->tx_done_waitq);
#endif
flushed:
	mdss_spi_display_notify(ctrl_pdata, MDP_NOTIFY_FRAME_FLUSHED);

	mutex_unlock(&ctrl_pdata->spi_tx_mutex);
	enable_spi_panel_te_irq(ctrl_pdata, false);

	return rc;
}

/* Plain FBIOPAN_DISPLAY / FBIOPUT_VSCREENINFO path (no overlay flags). */
static void mdss_spi_display_dma(struct msm_fb_data_type *mfd)
{
	mdss_spi_display_kickoff(mfd, NULL);
}

static int spi_display_get_metadata(struct msm_fb_data_type *mfd,
				struct msmfb_metadata *metadata)
{
	int ret = 0;

	switch (metadata->op) {
	case metadata_op_frame_rate:
		metadata->data.panel_frame_rate =
			mfd->panel_info->spi.frame_rate;
		break;
	case metadata_op_get_caps:
		metadata->data.caps.mdp_rev = 5;
		metadata->data.caps.rgb_pipes = 0;
		metadata->data.caps.vig_pipes = 0;
		metadata->data.caps.dma_pipes = 1;
		break;

	default:
		pr_warn("Unsupported request to GET META IOCTL %d\n",
			metadata->op);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int spi_display_ioctl_handler(struct msm_fb_data_type *mfd,
					  u32 cmd, void __user *argp)
{
	int val, ret = 0;
	struct mdss_panel_data *pdata;
	struct msmfb_metadata metadata;

	pdata = dev_get_platdata(&mfd->pdev->dev);

	switch (cmd) {
	case MSMFB_OVERLAY_VSYNC_CTRL:
		if (!copy_from_user(&val, argp, sizeof(val))) {
			mdss_spi_vsync_enable(pdata, val);
		} else {
			pr_err("overlay vsync ctrl copy from user failed\n");
			ret = -EFAULT;
		}
		break;
	case MSMFB_METADATA_GET:
		ret = copy_from_user(&metadata, argp, sizeof(metadata));
		if (ret) {
			pr_err("get metadata from user failed (%d)\n", ret);
			break;
		}
		ret = spi_display_get_metadata(mfd, &metadata);
		if (ret) {
			pr_err("spi_display_get_metadata failed (%d)\n", ret);
			break;
		}
		ret = copy_to_user(argp, &metadata, sizeof(metadata));
		if (ret)
			pr_err("copy to user failed (%d)\n", ret);
		break;
	default:
		break;
	}

	return ret;
}

static int mdss_spi_display_off(struct msm_fb_data_type *mfd)
{
	int rc = 0;
	struct mdss_panel_data *pdata;
	struct spi_panel_data *ctrl_pdata = NULL;

	pdata = dev_get_platdata(&mfd->pdev->dev);

	ctrl_pdata = container_of(pdata, struct spi_panel_data, panel_data);

	/* the kernel clock owns the cover: only Android's view goes off */
	if (s22_kclock_display_off(ctrl_pdata)) {
		mdss_spi_display_notifier_unregister(ctrl_pdata,
				&mfd->mdp_sync_pt_data.notifier);
		return 0;
	}

	ctrl_pdata->ctrl_state &= ~CTRL_STATE_PANEL_ACTIVE;

	if (ctrl_pdata->ctrl_state & CTRL_STATE_PANEL_INIT) {
		rc = mdss_spi_panel_off(&ctrl_pdata->panel_data);
		if (rc) {
			pr_err("%s: Panel off failed\n", __func__);
			s22_kclock_display_off_done(rc);
			return rc;
		}
		ctrl_pdata->ctrl_state &= ~CTRL_STATE_PANEL_INIT;
	}
	rc = mdss_spi_panel_power_ctrl(pdata, MDSS_PANEL_POWER_OFF);
	mdss_spi_display_notifier_unregister(ctrl_pdata,
			&mfd->mdp_sync_pt_data.notifier);
	s22_kclock_display_off_done(rc);

	return rc;
}

static int mdss_spi_display_on(struct msm_fb_data_type *mfd)
{
	int rc = 0;
	struct mdss_panel_data *pdata;
	struct spi_panel_data *ctrl_pdata = NULL;

	pdata = dev_get_platdata(&mfd->pdev->dev);

	ctrl_pdata = container_of(pdata, struct spi_panel_data, panel_data);

	/* the kernel clock owns the cover: only Android's view goes on */
	if (s22_kclock_display_on(ctrl_pdata)) {
		mdss_spi_display_notifier_register(ctrl_pdata,
				&mfd->mdp_sync_pt_data.notifier);
		return 0;
	}
	rc = mdss_spi_panel_power_ctrl(pdata, MDSS_PANEL_POWER_ON);
	if (rc) {
		pr_err("%s:Panel power on failed. rc=%d\n",
			__func__, rc);
		s22_kclock_display_on_done(rc);
		return rc;
	}

	mdss_spi_panel_pinctrl_set_state(ctrl_pdata, true);
	mdss_spi_panel_reset(pdata, 1);
	ctrl_pdata->ctrl_state |= CTRL_STATE_PANEL_ACTIVE;
	rc = mdss_spi_panel_on(&ctrl_pdata->panel_data);
	mdss_spi_display_notifier_register(ctrl_pdata,
			&mfd->mdp_sync_pt_data.notifier);
	s22_kclock_display_on_done(rc);

	return rc;
}

u32 mdss_spi_display_fb_stride(u32 fb_index, u32 xres, int bpp)
{
	/*
	 * The adreno GPU hardware requires that the pitch be aligned to
	 * 32 pixels for color buffers, so for the cases where the GPU
	 * is writing directly to fb0, the framebuffer pitch
	 * also needs to be 32 pixels aligned
	 */

	if (fb_index == 0)
		return ALIGN(xres, 32) * bpp;
	else
		return xres * bpp;
}

ssize_t mdss_spi_show_capabilities(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t len = PAGE_SIZE;
	int cnt = 0;

	cnt += scnprintf(buf + cnt, len - cnt, "mdp_version=5\n");
	cnt += scnprintf(buf + cnt, len - cnt, "hw_rev=%d\n", 5);
	cnt += scnprintf(buf + cnt, len - cnt, "pipe_count:%d\n", 1);
	cnt += scnprintf(buf + cnt, len - cnt,
		"pipe_num:3 pipe_type:rgb pipe_ndx:8 rects:1 pipe_is_handoff:0"
		);
	cnt += scnprintf(buf + cnt, len - cnt,
		"display_id:0 fmts_supported:51,224,0,22,0,191,248,255,1,");
	cnt += scnprintf(buf + cnt, len - cnt,
		"0,0,0,0,0,,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n");
	cnt += scnprintf(buf + cnt, len - cnt, "rgb_pipes=%d\n", 0);
	cnt += scnprintf(buf + cnt, len - cnt, "vig_pipes=%d\n", 0);
	cnt += scnprintf(buf + cnt, len - cnt, "dma_pipes=%d\n", 1);
	cnt += scnprintf(buf + cnt, len - cnt, "blending_stages=%d\n", 2);
	cnt += scnprintf(buf + cnt, len - cnt, "cursor_pipes=%d\n", 0);
	cnt += scnprintf(buf + cnt, len - cnt, "max_cursor_size=%d\n", 0);
	cnt += scnprintf(buf + cnt, len - cnt, "smp_count=%d\n", 0);
	cnt += scnprintf(buf + cnt, len - cnt, "smp_size=%d\n", 0);
	cnt += scnprintf(buf + cnt, len - cnt, "smp_mb_per_pipe=%d\n", 0);
	cnt += scnprintf(buf + cnt, len - cnt, "max_bandwidth_low=3100000\n");
	cnt += scnprintf(buf + cnt, len - cnt, "max_bandwidth_high=3100000\n");
	cnt += scnprintf(buf + cnt, len - cnt, "max_pipe_width=2048\n");
	cnt += scnprintf(buf + cnt, len - cnt, "max_mixer_width=2048\n");
	cnt += scnprintf(buf + cnt, len - cnt, "max_bandwidth_low=3100000\n");
	cnt += scnprintf(buf + cnt, len - cnt, "max_pipe_bw=2300000\n");
	cnt += scnprintf(buf + cnt, len - cnt, "max_mdp_clk=320000000\n");
	cnt += scnprintf(buf + cnt, len - cnt, "rot_dwnscale_min=1\n");
	cnt += scnprintf(buf + cnt, len - cnt, "rot_dwnscale_max=1\n");
	cnt += scnprintf(buf + cnt, len - cnt, "max_downscale_ratio=1\n");
	cnt += scnprintf(buf + cnt, len - cnt, "max_upscale_ratio=1\n");

	return cnt;
}

static ssize_t mdss_spi_vsync_show_event(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_panel_data *pdata;
	struct spi_panel_data *ctrl_pdata = NULL;
	int rc = 0;
	u64 vsync_ticks;

	pdata = dev_get_platdata(&mfd->pdev->dev);
	ctrl_pdata = container_of(pdata, struct spi_panel_data, panel_data);

	/* while the kernel clock owns the cover, answer as Android sees it */
	rc = s22_kclock_android_view(ctrl_pdata);
	if (!rc || (rc < 0 &&
		    !(ctrl_pdata->ctrl_state & CTRL_STATE_PANEL_ACTIVE)))
		return -EAGAIN;

	vsync_ticks = ktime_to_ns(ctrl_pdata->vsync_time);
	pr_debug("fb%d vsync=%llu\n", mfd->index, vsync_ticks);
	rc = scnprintf(buf, PAGE_SIZE, "VSYNC=%llu\n", vsync_ticks);

	return rc;
}

static int __vsync_retire_setup(struct msm_fb_data_type *mfd)
{
	char name[24];

	scnprintf(name, sizeof(name), "mdss_fb%d_retire", mfd->index);
	mfd->mdp_sync_pt_data.timeline_retire = mdss_create_timeline(name);
	if (mfd->mdp_sync_pt_data.timeline_retire == NULL) {
		pr_err("cannot vsync create time line\n");
		return -ENOMEM;
	}
	return 0;
}

static DEVICE_ATTR(vsync_event, 0444, mdss_spi_vsync_show_event, NULL);
static DEVICE_ATTR(caps, 0444, mdss_spi_show_capabilities, NULL);

static struct attribute *mdp_spi_sysfs_attrs[] = {
	&dev_attr_caps.attr,
	NULL,
};

static struct attribute *spi_vsync_fs_attr_group[] = {
	&dev_attr_vsync_event.attr,
	NULL,
};

static struct attribute_group mdp_spi_sysfs_group = {
	.attrs = mdp_spi_sysfs_attrs,
};

static struct attribute_group spi_vsync_sysfs_group = {
	.attrs = spi_vsync_fs_attr_group,
};

int mdss_spi_overlay_init(struct msm_fb_data_type *mfd)
{
	struct msm_mdp_interface *spi_display_interface = &mfd->mdp;
	struct device *dev = mfd->fbi->dev;
	struct mdss_data_type *spi_mdata;
	struct mdss_panel_data *pdata;
	struct spi_panel_data *ctrl_pdata = NULL;
	int rc = 0;

	pdata = dev_get_platdata(&mfd->pdev->dev);
	ctrl_pdata = container_of(pdata, struct spi_panel_data, panel_data);

	spi_mdata = dev_get_drvdata(mfd->pdev->dev.parent);
	if (!spi_mdata) {
		pr_err("unable to initialize spi mdata for fb%d\n", mfd->index);
		return -ENODEV;
	}

	spi_display_interface->on_fnc = mdss_spi_display_on;
	spi_display_interface->off_fnc = mdss_spi_display_off;
	spi_display_interface->do_histogram = NULL;
	spi_display_interface->cursor_update = NULL;

	spi_display_interface->ioctl_handler = spi_display_ioctl_handler;
	spi_display_interface->kickoff_fnc = mdss_spi_display_kickoff;
	spi_display_interface->dma_fnc = mdss_spi_display_dma;
	s22_kclock_attach(ctrl_pdata);
	spi_display_interface->pre_commit = mdss_spi_display_pre_commit;
	spi_display_interface->atomic_validate =
				mdss_spi_display_atomic_validate;
	spi_display_interface->fb_mem_get_iommu_domain = NULL;
	spi_display_interface->fb_stride = mdss_spi_display_fb_stride;
	spi_display_interface->fb_mem_alloc_fnc = NULL;
	spi_display_interface->check_dsi_status = NULL;

	BLOCKING_INIT_NOTIFIER_HEAD(&ctrl_pdata->notifier_head);

	rc = sysfs_create_group(&dev->kobj, &spi_vsync_sysfs_group);
	if (rc)
		pr_err("spi vsync sysfs group creation failed, ret=%d\n", rc);

	rc = sysfs_create_link_nowarn(&dev->kobj,
			&spi_mdata->pdev->dev.kobj, "mdp");
	if (rc)
		pr_warn("problem creating link to mdp sysfs\n");

	ctrl_pdata->vsync_event_sd = sysfs_get_dirent(dev->kobj.sd,
			"vsync_event");
	if (!ctrl_pdata->vsync_event_sd)
		pr_err("spi vsync_event sysfs lookup failed\n");

	rc = __vsync_retire_setup(mfd);
	if (IS_ERR_VALUE((unsigned long)rc))
		pr_err("unable to create vsync timeline\n");

	return rc;
}

static struct msm_mdp_interface spi_display_interface = {
	.init_fnc = mdss_spi_overlay_init,
	.fb_stride = mdss_spi_display_fb_stride,
};

int mdss_spi_display_register(void)
{
	return mdss_fb_register_spi_mdp_instance(&spi_display_interface);
}

static int mdss_spi_display_probe(struct platform_device *pdev)
{
	int rc = 0;
	struct mdss_data_type *mdata;
	struct device *dev = &pdev->dev;

	if (!pdev->dev.of_node) {
		pr_err("spi display driver only supports device tree probe\n");
		return -ENOTSUPP;
	}

	mdata = devm_kzalloc(&pdev->dev, sizeof(*mdata), GFP_KERNEL);
	if (mdata == NULL)
		return -ENOMEM;

	pdev->id = 0;
	mdata->pdev = pdev;
	platform_set_drvdata(pdev, mdata);

	/* The S22 has a DSI MDP for fb0 and an independent SPI MDP for fb2. */
	rc = mdss_spi_display_register();
	if (rc) {
		pr_err("unable to register SPI display instance\n");
		return rc;
	}

	rc = sysfs_create_group(&dev->kobj, &mdp_spi_sysfs_group);
	if (rc) {
		pr_err("spi vsync sysfs group creation failed, ret=%d\n", rc);
		return rc;
	}

	return 0;
}

static const struct of_device_id mdss_spi_display_match[] = {
	{ .compatible = "qcom,mdss-spi-panel" },
	{},
};

static struct platform_driver this_driver = {
	.probe = mdss_spi_display_probe,
	.driver = {
		.name = "spi_display",
		.owner  = THIS_MODULE,
		.of_match_table = mdss_spi_display_match,
	},
};

static int __init mdss_spi_panel_init(void)
{
	int ret;

	ret = platform_driver_register(&this_driver);
	return ret;
}

module_init(mdss_spi_panel_init);
MODULE_LICENSE("GPL v2");
MODULE_DEVICE_TABLE(of, mdss_spi_display_match);
