/*
 *  GPIO driven matrix keyboard driver
 *
 *  Copyright (c) 2008 Marek Vasut <marek.vasut@gmail.com>
 *
 *  Based on corgikbd.c
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 */

#include <linux/types.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/input/matrix_keypad.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>

/*
 * Debounce model (S22 Flip rework)
 * ================================
 *
 * The original driver sampled the matrix exactly once, msecs_to_jiffies(
 * debounce-delay-ms) after the first row edge, and then went back to
 * waiting for interrupts.  With HZ=100 msecs_to_jiffies(3..10) is one
 * jiffy, so that single sample lands at the next 10 ms tick plus the
 * system_wq latency, whatever the DT asked for.  Hence:
 *
 *  - a contact bounce that happens to be open at the sampling instant is
 *    reported as a release, and the re-close edge as a new press (phantom
 *    DOWN/UP pairs);
 *  - a press and release that both happen before that sample are never
 *    seen at all (lost taps).
 *
 * This version keeps the vendor matrix read (double read per row with
 * column direction switch, and the "all five columns active" anti-ghost
 * discard) but changes when it runs and how its result is used:
 *
 *  1. A row edge disables the row IRQs and samples the matrix immediately
 *     (no jiffy delay) from a SCHED_FIFO kthread worker, so the sample is
 *     taken close to the edge even when the CPUs are busy.
 *  2. While any key is unconfirmed (raw sample != reported state), or
 *     shortly after an edge, the matrix is re-sampled every
 *     settle_period_us using an hrtimer (real sub-ms resolution,
 *     independent of HZ).
 *  3. A key changes its reported state only after its raw level has been
 *     identical in every sample for at least debounce_us (per-key stable
 *     state debounce).  A raw level change restarts that key's timer.
 *  4. While keys are held and confirmed, the matrix is re-sampled every
 *     hold_period_ms so that releases and presses on a row that is already
 *     asserted are seen without relying on edges.
 *  5. Once nothing is pressed and everything is confirmed, all columns are
 *     re-activated and the row IRQs are enabled again.
 *
 * Optional DT properties (defaults used when absent):
 *   debounce-delay-ms         stable time required for a change (stock)
 *   bullitt,settle-period-us  sampling period while settling  (1000)
 *   bullitt,rescan-period-ms  sampling period while keys held (5)
 */
#define MATRIX_KEYPAD_SETTLE_US_DEFAULT	1000
#define MATRIX_KEYPAD_SETTLE_US_MIN	250
#define MATRIX_KEYPAD_SETTLE_US_MAX	10000
#define MATRIX_KEYPAD_HOLD_MS_DEFAULT	5
#define MATRIX_KEYPAD_HOLD_MS_MAX	50
#define MATRIX_KEYPAD_DEBOUNCE_MS_MAX	100

struct matrix_keypad {
	const struct matrix_keypad_platform_data *pdata;
	struct input_dev *input_dev;
	unsigned int row_shift;

	DECLARE_BITMAP(disabled_gpios, MATRIX_MAX_ROWS);

	/* debounced (reported) state, one bit per row for each column */
	uint32_t last_key_state[MATRIX_MAX_COLS];
	/* last raw sample, same layout */
	uint32_t raw_key_state[MATRIX_MAX_COLS];
	/* time of the last raw level change, per key (row * ncols + col) */
	ktime_t *raw_since;
	/* time of the last row edge (or of input open) */
	ktime_t edge_time;

	unsigned int debounce_us;
	unsigned int settle_period_us;
	unsigned int hold_period_us;

	struct kthread_worker *worker;	/* SCHED_FIFO scan thread */
	struct kthread_work scan_work;
	struct hrtimer scan_timer;
	spinlock_t lock;
	bool scan_pending;	/* polling: row IRQs are disabled */
	bool stopped;
	bool irqs_enabled;
	bool gpio_all_disabled;
};

/*
 * NOTE: normally the GPIO has to be put into HiZ when de-activated to cause
 * minmal side effect when scanning other columns, here it is configured to
 * be input, and it should work on most platforms.
 */
static void __activate_col(const struct matrix_keypad_platform_data *pdata,
			   int col, bool on)
{
	bool level_on = !pdata->active_low;

	if (on) {
		gpio_direction_output(pdata->col_gpios[col], level_on);
	} else {
		gpio_set_value_cansleep(pdata->col_gpios[col], !level_on);
		gpio_direction_input(pdata->col_gpios[col]);
	}
}

static void activate_col(const struct matrix_keypad_platform_data *pdata,
			 int col, bool on)
{
	__activate_col(pdata, col, on);

	if (on && pdata->col_scan_delay_us)
		udelay(pdata->col_scan_delay_us);
}

static void activate_all_cols(const struct matrix_keypad_platform_data *pdata,
			      bool on)
{
	int col;

	for (col = 0; col < pdata->num_col_gpios; col++)
		__activate_col(pdata, col, on);
}

static bool row_asserted(const struct matrix_keypad_platform_data *pdata,
			 int row)
{
	return gpio_get_value_cansleep(pdata->row_gpios[row]) ?
			!pdata->active_low : pdata->active_low;
}

/*
 * Row IRQ enable/disable.  The irqs_enabled flag keeps the enable/disable
 * depth balanced no matter which path (IRQ, scan end, stop, probe) gets
 * here first.  Callers hold keypad->lock, except probe/remove.
 */
static void enable_row_irqs(struct matrix_keypad *keypad)
{
	const struct matrix_keypad_platform_data *pdata = keypad->pdata;
	int i;

	if (keypad->irqs_enabled)
		return;

	if (pdata->clustered_irq > 0)
		enable_irq(pdata->clustered_irq);
	else {
		for (i = 0; i < pdata->num_row_gpios; i++)
			enable_irq(gpio_to_irq(pdata->row_gpios[i]));
	}
	keypad->irqs_enabled = true;
}

static void disable_row_irqs(struct matrix_keypad *keypad)
{
	const struct matrix_keypad_platform_data *pdata = keypad->pdata;
	int i;

	if (!keypad->irqs_enabled)
		return;

	if (pdata->clustered_irq > 0)
		disable_irq_nosync(pdata->clustered_irq);
	else {
		for (i = 0; i < pdata->num_row_gpios; i++)
			disable_irq_nosync(gpio_to_irq(pdata->row_gpios[i]));
	}
	keypad->irqs_enabled = false;
}

/*
 * Read the raw matrix into new_state (vendor read sequence, unchanged).
 *
 * For each row the column is driven active and the row is read, then the
 * column is driven low and the row is read again: a key only counts if the
 * row follows the column, which rejects rows that are stuck or leaking.
 * After each column all rows are briefly driven low to discharge them.
 *
 * Returns false when every column reports a key (vendor anti-ghost rule:
 * such a sample is treated as noise and must not change any key state).
 * Leaves all columns de-activated; the caller re-activates them.
 */
static bool matrix_keypad_read_matrix(struct matrix_keypad *keypad,
				      uint32_t *new_state)
{
	const struct matrix_keypad_platform_data *pdata = keypad->pdata;
	int row, col, count_state = 0;

	/* de-activate all columns for scanning */
	activate_all_cols(pdata, false);

	memset(new_state, 0, sizeof(uint32_t) * MATRIX_MAX_COLS);

	/* assert each column and read the row status out */
	for (col = 0; col < pdata->num_col_gpios; col++) {

		for (row = 0; row < pdata->num_row_gpios; row++) {
			activate_col(pdata, col, true);
			new_state[col] |=
				row_asserted(pdata, row) ? (1 << row) : 0;
			gpio_direction_output(pdata->col_gpios[col], 0);
			new_state[col] &=
				row_asserted(pdata, row) ? ~(1 << row) : ~(0);
		}
		if (new_state[col])
			count_state++;
		activate_col(pdata, col, false);
		for (row = 0; row < pdata->num_row_gpios; row++) {
			gpio_direction_output(pdata->row_gpios[row], 0);
			gpio_direction_input(pdata->row_gpios[row]);
		}
	}

	return count_state != 5;
}

static void matrix_keypad_arm_timer(struct matrix_keypad *keypad,
				    unsigned int period_us)
{
	hrtimer_start(&keypad->scan_timer,
		      ns_to_ktime((u64)period_us * NSEC_PER_USEC),
		      HRTIMER_MODE_REL);
}

enum matrix_keypad_key_result {
	MKP_KEY_STABLE,		/* reported state == raw state */
	MKP_KEY_SETTLING,	/* differs, not stable long enough yet */
	MKP_KEY_REPORTED,	/* change confirmed and reported */
};

/*
 * Per-key stable-state debounce.  'raw' is the bit of this key in the new
 * sample (0 or 1 << row).  A raw level change restarts the key's timer; a
 * difference from the reported state is only reported once the raw level
 * has been unchanged for at least debounce_us.
 */
static enum matrix_keypad_key_result
matrix_keypad_update_key(struct matrix_keypad *keypad, int row, int col,
			 uint32_t raw, ktime_t now)
{
	const unsigned short *keycodes = keypad->input_dev->keycode;
	ktime_t *since = &keypad->raw_since[row * keypad->pdata->num_col_gpios +
					    col];
	uint32_t bit = 1U << row;
	int code;

	if (raw != (keypad->raw_key_state[col] & bit)) {
		keypad->raw_key_state[col] ^= bit;
		*since = now;
	}

	if (raw == (keypad->last_key_state[col] & bit))
		return MKP_KEY_STABLE;

	if (ktime_us_delta(now, *since) < keypad->debounce_us)
		return MKP_KEY_SETTLING;

	code = MATRIX_SCAN_CODE(row, col, keypad->row_shift);
	input_event(keypad->input_dev, EV_MSC, MSC_SCAN, code);
	input_report_key(keypad->input_dev, keycodes[code], raw);
	keypad->last_key_state[col] ^= bit;

	return MKP_KEY_REPORTED;
}

/*
 * Sample the matrix, apply the per-key stable-state debounce, report
 * confirmed changes, then either schedule the next sample or go back to
 * interrupt mode.
 */
static void matrix_keypad_scan(struct kthread_work *work)
{
	struct matrix_keypad *keypad =
		container_of(work, struct matrix_keypad, scan_work);
	const struct matrix_keypad_platform_data *pdata = keypad->pdata;
	uint32_t new_state[MATRIX_MAX_COLS];
	bool valid, settling = false, held = false, reported = false;
	unsigned int period_us = 0;
	ktime_t now;
	int row, col;

	valid = matrix_keypad_read_matrix(keypad, new_state);
	now = ktime_get();

	/* idle level between samples: all columns active, as in IRQ mode */
	activate_all_cols(pdata, true);

	if (valid) {
		for (col = 0; col < pdata->num_col_gpios; col++) {
			if (new_state[col])
				held = true;

			for (row = 0; row < pdata->num_row_gpios; row++) {
				uint32_t raw = new_state[col] & (1U << row);
				enum matrix_keypad_key_result res;

				res = matrix_keypad_update_key(keypad, row, col,
							       raw, now);
				switch (res) {
				case MKP_KEY_SETTLING:
					settling = true;
					break;
				case MKP_KEY_REPORTED:
					reported = true;
					break;
				default:
					break;
				}
			}
		}
		if (reported)
			input_sync(keypad->input_dev);
	} else {
		/*
		 * Anti-ghost discard: this sample says nothing reliable.
		 * Keep the current states and timers and look again.
		 */
		settling = true;
	}

	/*
	 * Keep sampling quickly for a while after an edge even if this
	 * sample happened to catch an open bounce: a short tap whose first
	 * sample missed the contact is still seen by the next ones.
	 */
	if (ktime_us_delta(now, keypad->edge_time) <
	    keypad->debounce_us + 2 * keypad->settle_period_us)
		settling = true;

	if (settling)
		period_us = keypad->settle_period_us;
	else if (held)
		period_us = keypad->hold_period_us;

	spin_lock_irq(&keypad->lock);
	if (keypad->stopped) {
		/* matrix_keypad_stop() finishes the shutdown */
	} else if (period_us) {
		matrix_keypad_arm_timer(keypad, period_us);
	} else {
		/*
		 * Everything released and confirmed: back to interrupt mode.
		 * An edge that happened while the IRQs were disabled is
		 * replayed on enable (lazy disable + resend) and simply
		 * starts a new scan.
		 */
		keypad->scan_pending = false;
		enable_row_irqs(keypad);
	}
	spin_unlock_irq(&keypad->lock);
}

static enum hrtimer_restart matrix_keypad_timer(struct hrtimer *timer)
{
	struct matrix_keypad *keypad =
		container_of(timer, struct matrix_keypad, scan_timer);

	/* GPIO accesses may sleep (cansleep API): scan in process context */
	kthread_queue_work(keypad->worker, &keypad->scan_work);

	return HRTIMER_NORESTART;
}

static irqreturn_t matrix_keypad_interrupt(int irq, void *id)
{
	struct matrix_keypad *keypad = id;
	unsigned long flags;

	spin_lock_irqsave(&keypad->lock, flags);

	/*
	 * See if another IRQ beaten us to it and scheduled the
	 * scan already. In that case we should not try to
	 * disable IRQs again.
	 */
	if (unlikely(keypad->scan_pending || keypad->stopped))
		goto out;

	disable_row_irqs(keypad);
	keypad->scan_pending = true;
	keypad->edge_time = ktime_get();
	/* sample right away; the debounce is done by repeated samples */
	kthread_queue_work(keypad->worker, &keypad->scan_work);

out:
	spin_unlock_irqrestore(&keypad->lock, flags);
	return IRQ_HANDLED;
}

static int matrix_keypad_start(struct input_dev *dev)
{
	struct matrix_keypad *keypad = input_get_drvdata(dev);

	spin_lock_irq(&keypad->lock);
	keypad->stopped = false;
	keypad->scan_pending = true;
	keypad->edge_time = ktime_get();
	spin_unlock_irq(&keypad->lock);

	/*
	 * Schedule an immediate key scan to capture current key state;
	 * columns will be activated and IRQs be enabled once the state
	 * is confirmed and nothing is pressed.
	 */
	kthread_queue_work(keypad->worker, &keypad->scan_work);

	return 0;
}

static void matrix_keypad_stop(struct input_dev *dev)
{
	struct matrix_keypad *keypad = input_get_drvdata(dev);

	spin_lock_irq(&keypad->lock);
	keypad->stopped = true;
	spin_unlock_irq(&keypad->lock);

	/*
	 * After 'stopped' is set neither the IRQ handler nor the scan work
	 * arms anything new.  Kill a pending timer first (its callback may
	 * queue the work), then the work itself.
	 */
	hrtimer_cancel(&keypad->scan_timer);
	kthread_cancel_work_sync(&keypad->scan_work);

	/*
	 * Leave the matrix as the stock driver did after a scan: all columns
	 * active (so a key press pulls its row and can wake the system) and
	 * the row IRQs disabled (wake IRQs are armed by suspend).
	 */
	activate_all_cols(keypad->pdata, true);

	spin_lock_irq(&keypad->lock);
	disable_row_irqs(keypad);
	keypad->scan_pending = false;
	spin_unlock_irq(&keypad->lock);
}

#ifdef CONFIG_PM_SLEEP
static void matrix_keypad_enable_wakeup(struct matrix_keypad *keypad)
{
	const struct matrix_keypad_platform_data *pdata = keypad->pdata;
	unsigned int gpio;
	int i;

	if (pdata->clustered_irq > 0) {
		if (enable_irq_wake(pdata->clustered_irq) == 0)
			keypad->gpio_all_disabled = true;
	} else {

		for (i = 0; i < pdata->num_row_gpios; i++) {
			if (!test_bit(i, keypad->disabled_gpios)) {
				gpio = pdata->row_gpios[i];

				if (enable_irq_wake(gpio_to_irq(gpio)) == 0)
					__set_bit(i, keypad->disabled_gpios);
			}
		}
	}
}

static void matrix_keypad_disable_wakeup(struct matrix_keypad *keypad)
{
	const struct matrix_keypad_platform_data *pdata = keypad->pdata;
	unsigned int gpio;
	int i;

	if (pdata->clustered_irq > 0) {
		if (keypad->gpio_all_disabled) {
			disable_irq_wake(pdata->clustered_irq);
			keypad->gpio_all_disabled = false;
		}
	} else {
		for (i = 0; i < pdata->num_row_gpios; i++) {
			if (test_and_clear_bit(i, keypad->disabled_gpios)) {
				gpio = pdata->row_gpios[i];
				disable_irq_wake(gpio_to_irq(gpio));
			}
		}
	}
}

static int matrix_keypad_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct matrix_keypad *keypad = platform_get_drvdata(pdev);

	matrix_keypad_stop(keypad->input_dev);

	if (device_may_wakeup(&pdev->dev))
		matrix_keypad_enable_wakeup(keypad);

	return 0;
}

static int matrix_keypad_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct matrix_keypad *keypad = platform_get_drvdata(pdev);

	if (device_may_wakeup(&pdev->dev))
		matrix_keypad_disable_wakeup(keypad);

	matrix_keypad_start(keypad->input_dev);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(matrix_keypad_pm_ops,
			 matrix_keypad_suspend, matrix_keypad_resume);

static int matrix_keypad_init_gpio(struct platform_device *pdev,
				   struct matrix_keypad *keypad)
{
	const struct matrix_keypad_platform_data *pdata = keypad->pdata;
	int i, err;

	/* initialized strobe lines as outputs, activated */
	for (i = 0; i < pdata->num_col_gpios; i++) {
		err = gpio_request(pdata->col_gpios[i], "matrix_kbd_col");
		if (err) {
			dev_err(&pdev->dev,
				"failed to request GPIO%d for COL%d\n",
				pdata->col_gpios[i], i);
			goto err_free_cols;
		}

		gpio_direction_output(pdata->col_gpios[i], !pdata->active_low);
	}

	for (i = 0; i < pdata->num_row_gpios; i++) {
		err = gpio_request(pdata->row_gpios[i], "matrix_kbd_row");
		if (err) {
			dev_err(&pdev->dev,
				"failed to request GPIO%d for ROW%d\n",
				pdata->row_gpios[i], i);
			goto err_free_rows;
		}

		gpio_direction_input(pdata->row_gpios[i]);
	}

	if (pdata->clustered_irq > 0) {
		err = request_any_context_irq(pdata->clustered_irq,
				matrix_keypad_interrupt,
				pdata->clustered_irq_flags,
				"matrix-keypad", keypad);
		if (err < 0) {
			dev_err(&pdev->dev,
				"Unable to acquire clustered interrupt\n");
			goto err_free_rows;
		}
	} else {
		for (i = 0; i < pdata->num_row_gpios; i++) {
			err = request_any_context_irq(
					gpio_to_irq(pdata->row_gpios[i]),
					matrix_keypad_interrupt,
					IRQF_TRIGGER_RISING |
					IRQF_TRIGGER_FALLING,
					"matrix-keypad", keypad);
			if (err < 0) {
				dev_err(&pdev->dev,
					"Unable to acquire interrupt for GPIO line %i\n",
					pdata->row_gpios[i]);
				goto err_free_irqs;
			}
		}
	}

	/* initialized as disabled - enabled by input->open */
	keypad->irqs_enabled = true;
	disable_row_irqs(keypad);
	return 0;

err_free_irqs:
	while (--i >= 0)
		free_irq(gpio_to_irq(pdata->row_gpios[i]), keypad);
	i = pdata->num_row_gpios;
err_free_rows:
	while (--i >= 0)
		gpio_free(pdata->row_gpios[i]);
	i = pdata->num_col_gpios;
err_free_cols:
	while (--i >= 0)
		gpio_free(pdata->col_gpios[i]);

	return err;
}

static void matrix_keypad_free_gpio(struct matrix_keypad *keypad)
{
	const struct matrix_keypad_platform_data *pdata = keypad->pdata;
	int i;

	if (pdata->clustered_irq > 0) {
		free_irq(pdata->clustered_irq, keypad);
	} else {
		for (i = 0; i < pdata->num_row_gpios; i++)
			free_irq(gpio_to_irq(pdata->row_gpios[i]), keypad);
	}

	for (i = 0; i < pdata->num_row_gpios; i++)
		gpio_free(pdata->row_gpios[i]);

	for (i = 0; i < pdata->num_col_gpios; i++)
		gpio_free(pdata->col_gpios[i]);
}

#ifdef CONFIG_OF
static struct matrix_keypad_platform_data *
matrix_keypad_parse_dt(struct device *dev)
{
	struct matrix_keypad_platform_data *pdata;
	struct device_node *np = dev->of_node;
	unsigned int *gpios;
	int ret, i, nrow, ncol;

	if (!np) {
		dev_err(dev, "device lacks DT data\n");
		return ERR_PTR(-ENODEV);
	}

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		dev_err(dev, "could not allocate memory for platform data\n");
		return ERR_PTR(-ENOMEM);
	}

	pdata->num_row_gpios = nrow = of_gpio_named_count(np, "row-gpios");
	pdata->num_col_gpios = ncol = of_gpio_named_count(np, "col-gpios");
	if (nrow <= 0 || ncol <= 0) {
		dev_err(dev, "number of keypad rows/columns not specified\n");
		return ERR_PTR(-EINVAL);
	}

	if (of_get_property(np, "linux,no-autorepeat", NULL))
		pdata->no_autorepeat = true;

	pdata->wakeup = of_property_read_bool(np, "wakeup-source") ||
			of_property_read_bool(np, "linux,wakeup"); /* legacy */

	if (of_get_property(np, "gpio-activelow", NULL))
		pdata->active_low = true;

	pdata->name = of_get_property(np, "input-name", NULL);

	of_property_read_u32(np, "debounce-delay-ms", &pdata->debounce_ms);
	of_property_read_u32(np, "col-scan-delay-us",
						&pdata->col_scan_delay_us);

	gpios = devm_kzalloc(dev,
			     sizeof(unsigned int) *
				(pdata->num_row_gpios + pdata->num_col_gpios),
			     GFP_KERNEL);
	if (!gpios) {
		dev_err(dev, "could not allocate memory for gpios\n");
		return ERR_PTR(-ENOMEM);
	}

	for (i = 0; i < nrow; i++) {
		ret = of_get_named_gpio(np, "row-gpios", i);
		if (ret < 0)
			return ERR_PTR(ret);
		gpios[i] = ret;
	}

	for (i = 0; i < ncol; i++) {
		ret = of_get_named_gpio(np, "col-gpios", i);
		if (ret < 0)
			return ERR_PTR(ret);
		gpios[nrow + i] = ret;
	}

	pdata->row_gpios = gpios;
	pdata->col_gpios = &gpios[pdata->num_row_gpios];

	return pdata;
}
#else
static inline struct matrix_keypad_platform_data *
matrix_keypad_parse_dt(struct device *dev)
{
	dev_err(dev, "no platform data defined\n");

	return ERR_PTR(-EINVAL);
}
#endif

/*
 * Debounce/scan timing.  debounce-delay-ms keeps its stock meaning (how
 * long a change must be stable) but is now applied with hrtimer
 * resolution instead of being rounded to jiffies.
 */
static void matrix_keypad_parse_timing(struct device *dev,
				       struct matrix_keypad *keypad)
{
	unsigned int debounce_ms = keypad->pdata->debounce_ms;
	u32 settle_us = MATRIX_KEYPAD_SETTLE_US_DEFAULT;
	u32 hold_ms = MATRIX_KEYPAD_HOLD_MS_DEFAULT;

#ifdef CONFIG_OF
	if (dev->of_node) {
		of_property_read_u32(dev->of_node, "bullitt,settle-period-us",
				     &settle_us);
		of_property_read_u32(dev->of_node, "bullitt,rescan-period-ms",
				     &hold_ms);
	}
#endif

	debounce_ms = min_t(unsigned int, debounce_ms,
			    MATRIX_KEYPAD_DEBOUNCE_MS_MAX);
	settle_us = clamp_t(u32, settle_us, MATRIX_KEYPAD_SETTLE_US_MIN,
			    MATRIX_KEYPAD_SETTLE_US_MAX);
	hold_ms = clamp_t(u32, hold_ms, 1, MATRIX_KEYPAD_HOLD_MS_MAX);

	keypad->debounce_us = debounce_ms * USEC_PER_MSEC;
	keypad->settle_period_us = settle_us;
	keypad->hold_period_us = hold_ms * USEC_PER_MSEC;

	dev_info(dev, "debounce %u ms, settle period %u us, hold period %u ms\n",
		 debounce_ms, settle_us, hold_ms);
}

static int matrix_keypad_probe(struct platform_device *pdev)
{
	const struct matrix_keypad_platform_data *pdata;
	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO / 2 };
	struct matrix_keypad *keypad;
	struct input_dev *input_dev;
	int err;

	pdata = dev_get_platdata(&pdev->dev);
	if (!pdata) {
		pdata = matrix_keypad_parse_dt(&pdev->dev);
		if (IS_ERR(pdata))
			return PTR_ERR(pdata);
	} else if (!pdata->keymap_data) {
		dev_err(&pdev->dev, "no keymap data defined\n");
		return -EINVAL;
	}

	keypad = kzalloc(sizeof(struct matrix_keypad), GFP_KERNEL);
	input_dev = input_allocate_device();
	if (!keypad || !input_dev) {
		err = -ENOMEM;
		goto err_free_mem;
	}
	keypad->input_dev = input_dev;
	keypad->pdata = pdata;
	keypad->row_shift = get_count_order(pdata->num_col_gpios);
	keypad->stopped = true;
	kthread_init_work(&keypad->scan_work, matrix_keypad_scan);
	hrtimer_init(&keypad->scan_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	keypad->scan_timer.function = matrix_keypad_timer;
	spin_lock_init(&keypad->lock);

	keypad->raw_since = kcalloc(pdata->num_row_gpios *
				    pdata->num_col_gpios,
				    sizeof(*keypad->raw_since), GFP_KERNEL);
	if (!keypad->raw_since) {
		err = -ENOMEM;
		goto err_free_mem;
	}
	matrix_keypad_parse_timing(&pdev->dev, keypad);

	/*
	 * The scan runs on its own SCHED_FIFO thread (same class as threaded
	 * IRQ handlers) so that the first sample after an edge is not delayed
	 * by UI load; a press that ends before it is sampled is lost.
	 */
	keypad->worker = kthread_create_worker(0, "matrix_keypad");
	if (IS_ERR(keypad->worker)) {
		err = PTR_ERR(keypad->worker);
		keypad->worker = NULL;
		goto err_free_mem;
	}
	sched_setscheduler_nocheck(keypad->worker->task, SCHED_FIFO, &param);

	input_dev->name		= pdata->name ? : pdev->name;
	input_dev->id.bustype	= BUS_HOST;
	input_dev->dev.parent	= &pdev->dev;
	input_dev->open		= matrix_keypad_start;
	input_dev->close	= matrix_keypad_stop;

	err = matrix_keypad_build_keymap(pdata->keymap_data, NULL,
					 pdata->num_row_gpios,
					 pdata->num_col_gpios,
					 NULL, input_dev);
	if (err) {
		dev_err(&pdev->dev, "failed to build keymap\n");
		goto err_free_mem;
	}

	if (!pdata->no_autorepeat)
		__set_bit(EV_REP, input_dev->evbit);
	input_set_capability(input_dev, EV_MSC, MSC_SCAN);
	input_set_drvdata(input_dev, keypad);

	err = matrix_keypad_init_gpio(pdev, keypad);
	if (err)
		goto err_free_mem;

	err = input_register_device(keypad->input_dev);
	if (err)
		goto err_free_gpio;

	device_init_wakeup(&pdev->dev, pdata->wakeup);
	platform_set_drvdata(pdev, keypad);

	return 0;

err_free_gpio:
	matrix_keypad_free_gpio(keypad);
err_free_mem:
	input_free_device(input_dev);
	if (keypad) {
		if (keypad->worker)
			kthread_destroy_worker(keypad->worker);
		kfree(keypad->raw_since);
	}
	kfree(keypad);
	return err;
}

static int matrix_keypad_remove(struct platform_device *pdev)
{
	struct matrix_keypad *keypad = platform_get_drvdata(pdev);

	device_init_wakeup(&pdev->dev, 0);

	/* unregister first: close() stops the timer and the scan work */
	input_unregister_device(keypad->input_dev);
	hrtimer_cancel(&keypad->scan_timer);
	kthread_cancel_work_sync(&keypad->scan_work);
	matrix_keypad_free_gpio(keypad);
	kthread_destroy_worker(keypad->worker);
	kfree(keypad->raw_since);
	kfree(keypad);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id matrix_keypad_dt_match[] = {
	{ .compatible = "gpio-matrix-keypad" },
	{ }
};
MODULE_DEVICE_TABLE(of, matrix_keypad_dt_match);
#endif

static struct platform_driver matrix_keypad_driver = {
	.probe		= matrix_keypad_probe,
	.remove		= matrix_keypad_remove,
	.driver		= {
		.name	= "matrix-keypad",
		.pm	= &matrix_keypad_pm_ops,
		.of_match_table = of_match_ptr(matrix_keypad_dt_match),
	},
};
module_platform_driver(matrix_keypad_driver);

MODULE_AUTHOR("Marek Vasut <marek.vasut@gmail.com>");
MODULE_DESCRIPTION("GPIO Driven Matrix Keypad Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:matrix-keypad");
