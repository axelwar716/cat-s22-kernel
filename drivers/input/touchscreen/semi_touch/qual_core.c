/*
 * Qualcomm platform glue for the Chipsemi CHSC5xxx (semi_touch) driver,
 * adapted for the CAT S22 Flip (QM215, msm-4.9).
 *
 * Based on qual_core.c from HelloVolla/android_kernel_volla_mt6789
 * (volla-12.1, 0b0e1435e568), driver v3.6.2. Behaviour was aligned with the
 * stock S22 v30 kernel, recovered from its kallsyms and disassembly:
 *  - GPIOs from "chipsemi,int-gpio" / "chipsemi,rst-gpio", INT debounce 50;
 *  - framebuffer notifier on FB_EVENT_BLANK only, ignoring framebuffers
 *    with xres <= 239 (the 128x128 SPI cover display, fb2), as stock does;
 *  - suspend through the TP command (no power cycling), empty dev_pm_ops;
 *  - no proximity, gesture or APK debug node.
 * Deviation from stock: the "vdd"/"vio" supplies declared in the S22 device
 * tree are taken and enabled at probe (stock relied on other consumers of
 * pm8916_l17/l5 keeping them on). The Ontim "debug info" registration done
 * by the stock probe is not reproduced.
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/irq.h>
#include <linux/i2c.h>
#include <linux/regulator/consumer.h>

#include "head_def.h"
#include "semi_touch_device.h"
#include "semi_touch_custom.h"
#include "semi_touch_function.h"

#define semi_io_free(pin)   do { if (gpio_is_valid(pin)) gpio_free(pin); } while (0)

/* framebuffers narrower than this are the cover display: stock ignores them */
#define SEMI_TOUCH_MIN_FB_XRES   240

static const struct of_device_id sm_of_match[] = {
	{ .compatible = "chipsemi,chsc_cap_touch", },
	{ }
};
MODULE_DEVICE_TABLE(of, sm_of_match);

static const struct i2c_device_id sm_ts_id[] = {
	{ CHSC_DEVICE_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sm_ts_id);

static struct device_node *semi_touch_of_node(void)
{
	if (st_dev.client && st_dev.client->dev.of_node)
		return st_dev.client->dev.of_node;
	return of_find_matching_node(NULL, sm_of_match);
}

static int semi_touch_request_gpio(const char *prop, const char *label)
{
	struct device_node *np = semi_touch_of_node();
	int gpio, ret;

	check_return_if_zero(np, NULL);

	gpio = of_get_named_gpio(np, prop, 0);
	check_return_if_fail(gpio, NULL);

	ret = gpio_request(gpio, label);
	if (ret)
		kernel_log_e("gpio %d (%s) request failed: %d\n", gpio, label, ret);

	return gpio;
}

int semi_touch_get_int(void)
{
	return semi_touch_request_gpio("chipsemi,int-gpio", "chsc_int_pin");
}

int semi_touch_get_rst(void)
{
	return semi_touch_request_gpio("chipsemi,rst-gpio", "chsc_rst_pin");
}

int semi_touch_get_irq(int int_pin)
{
	gpio_set_debounce(int_pin, 50);

	return gpio_to_irq(int_pin);
}

static struct regulator *reg_vdd;
static struct regulator *reg_vio;

static struct regulator *semi_touch_supply_on(struct device *dev, const char *id)
{
	struct regulator *reg = devm_regulator_get_optional(dev, id);
	int ret;

	if (IS_ERR(reg)) {
		if (PTR_ERR(reg) == -EPROBE_DEFER)
			return reg;
		kernel_log_d("%s supply not available (%ld)\n", id, PTR_ERR(reg));
		return NULL;
	}

	ret = regulator_enable(reg);
	if (ret) {
		kernel_log_e("%s supply enable failed: %d\n", id, ret);
		return NULL;
	}

	return reg;
}

static int semi_touch_power_init(struct i2c_client *client)
{
	reg_vdd = semi_touch_supply_on(&client->dev, "vdd");
	if (IS_ERR(reg_vdd))
		return PTR_ERR(reg_vdd);

	reg_vio = semi_touch_supply_on(&client->dev, "vio");
	if (IS_ERR(reg_vio)) {
		if (reg_vdd)
			regulator_disable(reg_vdd);
		reg_vdd = NULL;
		return PTR_ERR(reg_vio);
	}

	return 0;
}

static void semi_touch_power_exit(void)
{
	if (reg_vio)
		regulator_disable(reg_vio);
	if (reg_vdd)
		regulator_disable(reg_vdd);
	reg_vio = NULL;
	reg_vdd = NULL;
}

int semi_touch_platform_variety(void)
{
	if (st_dev.int_pin)
		semi_io_free(st_dev.int_pin);

	if (st_dev.rst_pin)
		semi_io_free(st_dev.rst_pin);

	return 0;
}

/* CONFIG_FB is set on the S22, so PM is driven by the fb notifier (stock) */
static const struct dev_pm_ops semi_touch_dev_pm_ops = {
};

#include <linux/notifier.h>
#include <linux/fb.h>
struct notifier_block sm_fb_notify;

static int semi_touch_fb_notifier_callback(struct notifier_block *self,
					   unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;

	if (!evdata || !evdata->info || !evdata->data)
		return 0;

	if (evdata->info->var.xres < SEMI_TOUCH_MIN_FB_XRES) {
		pr_debug("tp recive fb%d event , don't care\n", evdata->info->node);
		return 0;
	}

	if (FB_EVENT_BLANK != event || !st_dev.client)
		return 0;

	blank = evdata->data;
	if (FB_BLANK_UNBLANK == *blank)
		semi_touch_resume_entry(&st_dev.client->dev);
	else if (FB_BLANK_POWERDOWN == *blank)
		semi_touch_suspend_entry(&st_dev.client->dev);

	return 0;
}

int semi_touch_work_done(void)
{
	int ret;

	sm_fb_notify.notifier_call = semi_touch_fb_notifier_callback;
	ret = fb_register_client(&sm_fb_notify);
	check_return_if_fail(ret, NULL);

	return ret;
}

int semi_touch_resource_release(void)
{
	fb_unregister_client(&sm_fb_notify);
	return semi_touch_platform_variety();
}

static int semi_touch_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret;

	ret = semi_touch_power_init(client);
	if (ret)
		return ret;

	ret = semi_touch_init(client);
	if (ret) {
		kernel_log_e("semitouch probe failed: %d\n", ret);
		semi_touch_deinit(client);
		semi_touch_power_exit();
		return ret;
	}

	kernel_log_d("semitouch probe finished, driver ver(%s)\n", CHSC_DRIVER_VERSION);

	return 0;
}

static int semi_touch_remove(struct i2c_client *client)
{
	int ret = semi_touch_deinit(client);

	semi_touch_power_exit();

	return ret;
}

int semi_touch_suspend_entry(struct device *dev)
{
	if (is_proximity_function_en(st_dev.stc.custom_function_en) &&
	    is_proximity_activate(st_dev.stc.ctp_run_status)) {
		kernel_log_d("proximity is active, so fake suspend...\n");
		return SEMI_DRV_ERR_OK;
	}

	if (is_guesture_function_en(st_dev.stc.custom_function_en)) {
		semi_touch_guesture_switch(1);
	} else {
		semi_touch_suspend_ctrl(1);
		semi_touch_clear_report();
		kernel_log_d("tpd real suspend...\n");
	}

	return SEMI_DRV_ERR_OK;
}

int semi_touch_resume_entry(struct device *dev)
{
	unsigned char bootCheckOk = 0;
	unsigned char glove_activity = is_glove_activate(st_dev.stc.ctp_run_status);

	if (is_proximity_activate(st_dev.stc.ctp_run_status) &&
	    is_proximity_function_en(st_dev.stc.custom_function_en)) {
		kernel_log_d("proximity is active, so fake resume...\n");
		return SEMI_DRV_ERR_OK;
	}

	/* reset tp + iic detected */
	semi_touch_reset_and_detect();
	semi_touch_clear_report();

	if (glove_activity) {
		semi_touch_start_up_check(&bootCheckOk);
		if (bootCheckOk)
			semi_touch_glove_switch(1);
	}
	kernel_log_d("tpd_resume...\n");

	return SEMI_DRV_ERR_OK;
}

static struct i2c_driver sm_touch_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name  = CHSC_DEVICE_NAME,
		.of_match_table = of_match_ptr(sm_of_match),
#ifdef CONFIG_PM
		.pm = &semi_touch_dev_pm_ops,
#endif
	},
	.id_table = sm_ts_id,
	.probe = semi_touch_probe,
	.remove = semi_touch_remove,
};

module_i2c_driver(sm_touch_driver);

MODULE_DESCRIPTION("Chipsemi CHSC5xxx (semi_touch) touchscreen driver, CAT S22 Flip port");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("wasim");
