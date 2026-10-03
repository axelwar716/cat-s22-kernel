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
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/of_address.h>
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
 *  3. Per-key press/release rules (v14):
 *       - press:   DOWN once the key has been closed for press_us in
 *                  total within one contact burst.  Openings shorter
 *                  than release_us do not end the burst, so a light or
 *                  bouncy tap whose closed pieces are each shorter than
 *                  press_us still counts, and a make bounce plus the
 *                  real contact is still ONE press.  (v8-v13 required
 *                  press_us *continuously* closed: light taps were lost,
 *                  03-10 20:34 and 20:41, row IRQ seen and no DOWN.)
 *       - release: UP once open continuously for release_us (mid-press
 *                  openings of 4-22 ms are not releases).
 *       - a burst that opens for release_us without reaching press_us
 *         is dropped (counted in the rejected_taps parameter).
 *     Each is max(debounce-delay-ms, its own period), so the stock
 *     debounce-delay-ms (3 ms in the S22 DTBO) can only lengthen them.
 *  4. While keys are held and confirmed, the matrix is re-sampled every
 *     hold_period_ms so that releases and presses on a row that is already
 *     asserted are seen without relying on edges.
 *  5. Once nothing is pressed and everything is confirmed, all columns are
 *     re-activated and the row IRQs are enabled again (idle costs nothing:
 *     no polling).  Right after the re-arm the rows are read once more; a
 *     contact that started between the last sample and the re-arm starts
 *     a new scan instead of depending on the dual-edge emulation of
 *     pinctrl-msm.  Desk test: polling at a fixed period after typing
 *     instead of re-arming the IRQs loses more light taps (the IRQ sees
 *     the very first edge, a poll only the next sample).
 *
 *  6. Row bias (v16).  The rows idle low through the TLMM internal
 *     pull-down.  A dome pressed lightly has a high contact resistance
 *     that forms a divider with that pull-down, so the row may never
 *     cross the input threshold: no edge and no read (lost taps on the
 *     4 and the 7 with no IRQ at all, 03-10).  row_pull selects the row
 *     bias written straight into GPIO_CFG: 0 = none, 1 = pull-down
 *     (stock), 2 = keeper.  With 0 or 2 nothing pulls the row low, so the
 *     driver discharges it itself (row output low for 1 us) before every
 *     column read and before re-arming the IRQs.  The pull-down is put
 *     back while suspended, so a floating row cannot wake the system.
 *     The matrix read also waits fall_us after dropping the column
 *     before checking that the row followed it (v8-v15: no wait).
 *
 * Runtime tuning (0 = DT/default):
 *   /sys/module/matrix_keypad/parameters/press_ms
 *   /sys/module/matrix_keypad/parameters/release_ms
 * Runtime tuning (raw values): row_pull (0/1/2), fall_us.
 * Counters (read only): rejected_taps, rearm_rescans, follow_fail
 * (row high with its column driven and still high fall_us after the
 * column dropped), irq_no_key (row IRQ whose scan never saw a key closed;
 * the software resend right after each re-arm is not counted).
 *
 * Optional DT properties (defaults used when absent):
 *   debounce-delay-ms         minimum stable time for any change (stock)
 *   bullitt,press-period-ms   total closed time before DOWN  (6)
 *   bullitt,release-period-ms stable open time before UP     (30)
 *   bullitt,settle-period-us  sampling period while settling  (1000)
 *   bullitt,rescan-period-ms  sampling period while keys held (5)
 */
#define MATRIX_KEYPAD_SETTLE_US_DEFAULT	1000
#define MATRIX_KEYPAD_SETTLE_US_MIN	250
#define MATRIX_KEYPAD_SETTLE_US_MAX	10000
#define MATRIX_KEYPAD_HOLD_MS_DEFAULT	5
#define MATRIX_KEYPAD_HOLD_MS_MAX	50
#define MATRIX_KEYPAD_DEBOUNCE_MS_MAX	100
/*
 * Measured on the S22 Flip dome contacts: make bounces of 11-18 ms closed
 * followed by 9-18 ms open before the real contact, and openings of
 * 4-22 ms in the middle of a 100-170 ms press.  The release hysteresis
 * merges both into one press; the press threshold only rejects contacts
 * shorter than 6 ms in total (desk test tools/keypad-v14-accum-test.py:
 * 0 lost / 0 phantom on the 03-10 captures, 0/2000 lost light taps).
 */
#define MATRIX_KEYPAD_PRESS_MS_DEFAULT	6
#define MATRIX_KEYPAD_RELEASE_MS_DEFAULT	30

static unsigned int param_press_ms;
module_param_named(press_ms, param_press_ms, uint, 0644);
MODULE_PARM_DESC(press_ms, "total closed time before DOWN, ms (0 = DT/default)");

static unsigned int param_release_ms;
module_param_named(release_ms, param_release_ms, uint, 0644);
MODULE_PARM_DESC(release_ms, "continuous open time before UP, ms (0 = DT/default)");

static unsigned int rejected_taps;
module_param(rejected_taps, uint, 0444);
MODULE_PARM_DESC(rejected_taps, "contact bursts shorter than press_ms");

static unsigned int rearm_rescans;
module_param(rearm_rescans, uint, 0444);
MODULE_PARM_DESC(rearm_rescans, "scans started by the row check after the IRQ re-arm");

/* TLMM GPIO_CFG / GPIO_IN_OUT, msm8917/msm8937 layout (pinctrl-msm89x7.c) */
#define TLMM_GPIO_REG_SIZE	0x1000
#define TLMM_GPIO_IO_OFFSET	0x4
#define TLMM_CFG_PULL_MASK	0x3
#define TLMM_CFG_OE		BIT(9)
#define TLMM_PULL_NONE		0
#define TLMM_PULL_DOWN		1
#define TLMM_PULL_KEEPER	2

/* an IRQ this soon after a re-arm is the software resend of a latched edge */
#define MATRIX_KEYPAD_RESEND_US	5000
#define MATRIX_KEYPAD_FALL_US_MAX	100

static unsigned int param_row_pull = TLMM_PULL_NONE;
module_param_named(row_pull, param_row_pull, uint, 0644);
MODULE_PARM_DESC(row_pull, "row bias: 0 none (driver discharges the rows), 1 pull-down (stock), 2 keeper");

static unsigned int param_fall_us = 5;
module_param_named(fall_us, param_fall_us, uint, 0644);
MODULE_PARM_DESC(fall_us, "wait after dropping a column before the row-follows check, us");

static unsigned int follow_fail;
module_param(follow_fail, uint, 0444);
MODULE_PARM_DESC(follow_fail, "rows that rose with their column but did not fall with it");

static unsigned int irq_no_key;
module_param(irq_no_key, uint, 0444);
MODULE_PARM_DESC(irq_no_key, "row IRQs whose scan never saw a key closed");

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
	/* closed time in the current contact burst, per key, us */
	u32 *press_acc;
	/* time of the last valid sample */
	ktime_t last_scan;
	/* time of the last row edge (or of input open) */
	ktime_t edge_time;

	unsigned int press_us;		/* total closed time before DOWN */
	unsigned int release_us;	/* open time before UP */
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
	bool rearm_rescan;	/* last scan was started by the re-arm check */

	/* per-row TLMM registers (NULL: row bias stays as the boot left it) */
	void __iomem **row_cfg;
	void __iomem **row_io;
	unsigned int row_pull;	/* bias currently written to the rows */
	ktime_t rearm_time;	/* last switch back to interrupt mode */
	bool irq_burst;		/* this scan sequence was started by a row IRQ */
	bool burst_contact;	/* ... and some sample saw a key closed */
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

static bool any_row_asserted(const struct matrix_keypad_platform_data *pdata)
{
	int row;

	for (row = 0; row < pdata->num_row_gpios; row++)
		if (row_asserted(pdata, row))
			return true;

	return false;
}

static uint32_t read_rows(const struct matrix_keypad_platform_data *pdata)
{
	uint32_t rows = 0;
	int row;

	for (row = 0; row < pdata->num_row_gpios; row++)
		if (row_asserted(pdata, row))
			rows |= 1U << row;

	return rows;
}

/*
 * Write the row bias straight into GPIO_CFG.  The rows are requested as
 * IRQs, so neither gpiolib nor pinctrl lets the driver touch them, and
 * the keypad node has no pinctrl states that could rewrite them later.
 */
static void matrix_keypad_set_row_pull(struct matrix_keypad *keypad,
				       unsigned int pull)
{
	int row;
	u32 ctl;

	if (!keypad->row_cfg || pull == keypad->row_pull)
		return;

	for (row = 0; row < keypad->pdata->num_row_gpios; row++) {
		ctl = readl(keypad->row_cfg[row]);
		writel((ctl & ~TLMM_CFG_PULL_MASK) | pull, keypad->row_cfg[row]);
	}
	keypad->row_pull = pull;
}

/*
 * Without the pull-down a row keeps whatever charge its last contact left.
 * Drive every row low for 1 us (OUT is kept at 0, only OE is toggled) and
 * let it float again.  Called with the IRQs disabled: the falling edge is
 * latched and replayed on re-arm like any other.
 */
static void matrix_keypad_discharge_rows(struct matrix_keypad *keypad)
{
	int row, nrow = keypad->pdata->num_row_gpios;

	if (!keypad->row_cfg || keypad->row_pull == TLMM_PULL_DOWN)
		return;

	for (row = 0; row < nrow; row++)
		writel(readl(keypad->row_cfg[row]) | TLMM_CFG_OE,
		       keypad->row_cfg[row]);
	udelay(1);
	for (row = 0; row < nrow; row++)
		writel(readl(keypad->row_cfg[row]) & ~TLMM_CFG_OE,
		       keypad->row_cfg[row]);
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
 * Read the raw matrix into new_state (vendor read rule, one pass per
 * column instead of one per key since v16).
 *
 * The column is driven active and the rows are read, then the column is
 * driven low and, fall_us later, the rows are read again: a key only
 * counts if its row follows the column, which rejects rows that are stuck
 * or leaking.  Without the row pull-down the rows are discharged before
 * each column, so a row can only read high if this column charged it.
 *
 * Returns false when every column reports a key (vendor anti-ghost rule:
 * such a sample is treated as noise and must not change any key state).
 * Leaves all columns de-activated; the caller re-activates them.
 */
static bool matrix_keypad_read_matrix(struct matrix_keypad *keypad,
				      uint32_t *new_state, unsigned int fall_us)
{
	const struct matrix_keypad_platform_data *pdata = keypad->pdata;
	int col, count_state = 0;
	uint32_t high, still_high;

	/* de-activate all columns for scanning */
	activate_all_cols(pdata, false);

	memset(new_state, 0, sizeof(uint32_t) * MATRIX_MAX_COLS);

	/* assert each column and read the row status out */
	for (col = 0; col < pdata->num_col_gpios; col++) {
		matrix_keypad_discharge_rows(keypad);
		activate_col(pdata, col, true);
		high = read_rows(pdata);
		gpio_direction_output(pdata->col_gpios[col], 0);
		if (high && fall_us)
			udelay(fall_us);
		still_high = high ? read_rows(pdata) : 0;
		new_state[col] = high & ~still_high;
		if (high & still_high)
			follow_fail += hweight32(high & still_high);
		if (new_state[col])
			count_state++;
		activate_col(pdata, col, false);
		/*
		 * The vendor code drove every row low here through gpiolib,
		 * which refuses for IRQ lines (FLAG_USED_AS_IRQ: -EIO plus one
		 * error line per row, a log flood with 1 ms rescans).  v16
		 * does it through GPIO_CFG, see matrix_keypad_discharge_rows().
		 */
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
 * Per-key press/release rules (see the top of the file).  'raw' is the bit
 * of this key in the new sample (0 or 1 << row), dt_us the time since the
 * previous valid sample.
 */
static enum matrix_keypad_key_result
matrix_keypad_update_key(struct matrix_keypad *keypad, int row, int col,
			 uint32_t raw, ktime_t now, u32 dt_us,
			 unsigned int press_us, unsigned int release_us)
{
	const unsigned short *keycodes = keypad->input_dev->keycode;
	unsigned int idx = row * keypad->pdata->num_col_gpios + col;
	ktime_t *since = &keypad->raw_since[idx];
	u32 *acc = &keypad->press_acc[idx];
	uint32_t bit = 1U << row;
	int code;

	if (raw != (keypad->raw_key_state[col] & bit)) {
		keypad->raw_key_state[col] ^= bit;
		*since = now;
	}

	if (keypad->last_key_state[col] & bit) {
		/* reported DOWN: UP after release_us open continuously */
		if (raw)
			return MKP_KEY_STABLE;
		if (ktime_us_delta(now, *since) < release_us)
			return MKP_KEY_SETTLING;
	} else if (raw) {
		/* reported UP, closed: DOWN once the burst adds up */
		*acc += dt_us;
		if (*acc < press_us)
			return MKP_KEY_SETTLING;
		*acc = 0;
	} else {
		/* reported UP, open: end of a burst that did not add up? */
		if (!*acc)
			return MKP_KEY_STABLE;
		if (ktime_us_delta(now, *since) < release_us)
			return MKP_KEY_SETTLING;
		*acc = 0;
		rejected_taps++;
		return MKP_KEY_STABLE;
	}

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
	bool rearmed = false;
	unsigned int period_us = 0, press_us, release_us, ms, pull;
	u32 dt_us;
	ktime_t now;
	int row, col;

	ms = READ_ONCE(param_press_ms);
	press_us = ms ? min_t(unsigned int, ms, MATRIX_KEYPAD_DEBOUNCE_MS_MAX) *
			USEC_PER_MSEC : keypad->press_us;
	ms = READ_ONCE(param_release_ms);
	release_us = ms ? min_t(unsigned int, ms, MATRIX_KEYPAD_DEBOUNCE_MS_MAX) *
			  USEC_PER_MSEC : keypad->release_us;
	pull = READ_ONCE(param_row_pull);
	if (pull > TLMM_PULL_KEEPER)
		pull = TLMM_PULL_DOWN;
	matrix_keypad_set_row_pull(keypad, pull);

	valid = matrix_keypad_read_matrix(keypad, new_state,
			min_t(unsigned int, READ_ONCE(param_fall_us),
			      MATRIX_KEYPAD_FALL_US_MAX));
	now = ktime_get();

	if (valid) {
		/*
		 * Closed time credited to this sample: the time since the
		 * previous one, capped so that the first sample of a burst
		 * (after an idle IRQ wait) only counts as one period or two.
		 */
		dt_us = min_t(s64, ktime_us_delta(now, keypad->last_scan),
			      2 * keypad->settle_period_us);
		keypad->last_scan = now;

		for (col = 0; col < pdata->num_col_gpios; col++) {
			if (new_state[col]) {
				held = true;
				keypad->burst_contact = true;
			}

			for (row = 0; row < pdata->num_row_gpios; row++) {
				uint32_t raw = new_state[col] & (1U << row);
				enum matrix_keypad_key_result res;

				res = matrix_keypad_update_key(keypad, row, col,
							       raw, now, dt_us,
							       press_us,
							       release_us);
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
	    max(press_us, release_us) + 2 * keypad->settle_period_us)
		settling = true;

	if (held)
		keypad->rearm_rescan = false;

	if (settling)
		period_us = keypad->settle_period_us;
	else if (held)
		period_us = keypad->hold_period_us;

	/*
	 * Idle level between samples: all columns active, as in IRQ mode.
	 * Before going back to interrupt mode a floating row must start low
	 * (with the columns still off), or a light press has no edge to make.
	 */
	if (!period_us)
		matrix_keypad_discharge_rows(keypad);
	activate_all_cols(pdata, true);

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
		if (keypad->irq_burst && !keypad->burst_contact)
			irq_no_key++;
		keypad->irq_burst = false;
		keypad->rearm_time = ktime_get();
		keypad->scan_pending = false;
		enable_row_irqs(keypad);
		rearmed = true;
	}
	spin_unlock_irq(&keypad->lock);

	/*
	 * Close the window between the last sample and the re-arm: with all
	 * columns active an asserted row means a contact started in between.
	 * Its edge should be latched and replayed, but the row IRQs are
	 * dual-edge emulated by pinctrl-msm (polarity only follows the line
	 * on ack), so do not rely on it.  Once per quiet re-arm: a row stuck
	 * asserted with no key in the matrix read must not spin.
	 */
	if (!rearmed || keypad->rearm_rescan)
		return;
	if (pdata->col_scan_delay_us)
		udelay(pdata->col_scan_delay_us);
	if (!any_row_asserted(pdata))
		return;

	spin_lock_irq(&keypad->lock);
	if (!keypad->scan_pending && !keypad->stopped) {
		disable_row_irqs(keypad);
		keypad->scan_pending = true;
		keypad->rearm_rescan = true;
		keypad->edge_time = ktime_get();
		rearm_rescans++;
		kthread_queue_work(keypad->worker, &keypad->scan_work);
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
	keypad->rearm_rescan = false;
	keypad->edge_time = ktime_get();
	keypad->irq_burst = ktime_us_delta(keypad->edge_time,
					   keypad->rearm_time) >=
			    MATRIX_KEYPAD_RESEND_US;
	keypad->burst_contact = false;
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
	 * the row IRQs disabled (wake IRQs are armed by suspend).  The rows
	 * get the stock pull-down back: a floating wake line could leak up
	 * and wake the system.  The next scan restores row_pull.
	 */
	matrix_keypad_set_row_pull(keypad, TLMM_PULL_DOWN);
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
 * resolution instead of being rounded to jiffies; it is a floor for the
 * press and release periods, which default to values measured on the S22
 * so that the stock DTBO (debounce-delay-ms = 3) needs no change.
 */
static void matrix_keypad_parse_timing(struct device *dev,
				       struct matrix_keypad *keypad)
{
	unsigned int debounce_ms = keypad->pdata->debounce_ms;
	u32 settle_us = MATRIX_KEYPAD_SETTLE_US_DEFAULT;
	u32 hold_ms = MATRIX_KEYPAD_HOLD_MS_DEFAULT;
	u32 press_ms = MATRIX_KEYPAD_PRESS_MS_DEFAULT;
	u32 release_ms = MATRIX_KEYPAD_RELEASE_MS_DEFAULT;

#ifdef CONFIG_OF
	if (dev->of_node) {
		of_property_read_u32(dev->of_node, "bullitt,settle-period-us",
				     &settle_us);
		of_property_read_u32(dev->of_node, "bullitt,rescan-period-ms",
				     &hold_ms);
		of_property_read_u32(dev->of_node, "bullitt,press-period-ms",
				     &press_ms);
		of_property_read_u32(dev->of_node, "bullitt,release-period-ms",
				     &release_ms);
	}
#endif

	debounce_ms = min_t(unsigned int, debounce_ms,
			    MATRIX_KEYPAD_DEBOUNCE_MS_MAX);
	settle_us = clamp_t(u32, settle_us, MATRIX_KEYPAD_SETTLE_US_MIN,
			    MATRIX_KEYPAD_SETTLE_US_MAX);
	hold_ms = clamp_t(u32, hold_ms, 1, MATRIX_KEYPAD_HOLD_MS_MAX);
	press_ms = clamp_t(u32, press_ms, debounce_ms,
			   MATRIX_KEYPAD_DEBOUNCE_MS_MAX);
	release_ms = clamp_t(u32, release_ms, debounce_ms,
			     MATRIX_KEYPAD_DEBOUNCE_MS_MAX);

	keypad->press_us = press_ms * USEC_PER_MSEC;
	keypad->release_us = release_ms * USEC_PER_MSEC;
	keypad->settle_period_us = settle_us;
	keypad->hold_period_us = hold_ms * USEC_PER_MSEC;

	dev_info(dev, "debounce %u ms, press %u ms (total), release %u ms, settle period %u us, hold period %u ms\n",
		 debounce_ms, press_ms, release_ms, settle_us, hold_ms);
}

/*
 * Map GPIO_CFG and GPIO_IN_OUT of each row (one page per row) from the
 * TLMM node that owns the row GPIOs.  On failure row_pull and the row
 * discharge are disabled and the driver behaves as v15.
 */
static int matrix_keypad_map_rows(struct device *dev,
				  struct matrix_keypad *keypad)
{
	const struct matrix_keypad_platform_data *pdata = keypad->pdata;
	struct device_node *np = NULL;
	struct gpio_chip *chip = NULL;
	struct resource res;
	unsigned int hw;
	int row, nrow = pdata->num_row_gpios;

	keypad->row_pull = TLMM_PULL_DOWN;

	for (row = 0; row < nrow; row++) {
		struct gpio_desc *desc = gpio_to_desc(pdata->row_gpios[row]);
		struct gpio_chip *c = desc ? gpiod_to_chip(desc) : NULL;

		if (!c || (chip && c != chip))
			return -ENODEV;
		chip = c;
	}
	np = chip->of_node ? : (chip->parent ? chip->parent->of_node : NULL);
	if (!np || of_address_to_resource(np, 0, &res))
		return -ENODEV;

	keypad->row_cfg = devm_kcalloc(dev, nrow, sizeof(void __iomem *),
				       GFP_KERNEL);
	keypad->row_io = devm_kcalloc(dev, nrow, sizeof(void __iomem *),
				      GFP_KERNEL);
	if (!keypad->row_cfg || !keypad->row_io)
		goto fail;

	for (row = 0; row < nrow; row++) {
		hw = pdata->row_gpios[row] - chip->base;
		if (hw >= chip->ngpio ||
		    (u64)(hw + 1) * TLMM_GPIO_REG_SIZE > resource_size(&res))
			goto fail;
		keypad->row_cfg[row] = devm_ioremap(dev,
				res.start + hw * TLMM_GPIO_REG_SIZE,
				TLMM_GPIO_IO_OFFSET + 4);
		if (!keypad->row_cfg[row])
			goto fail;
		keypad->row_io[row] = keypad->row_cfg[row] +
				      TLMM_GPIO_IO_OFFSET;
	}

	/* OUT stays 0: the discharge only toggles OE */
	for (row = 0; row < nrow; row++)
		writel(0, keypad->row_io[row]);
	keypad->row_pull = readl(keypad->row_cfg[0]) & TLMM_CFG_PULL_MASK;
	dev_info(dev, "rows at %pa, boot row bias %u, row_pull %u, fall %u us\n",
		 &res.start, keypad->row_pull, param_row_pull, param_fall_us);
	return 0;

fail:
	keypad->row_cfg = NULL;
	keypad->row_pull = TLMM_PULL_DOWN;
	return -ENOMEM;
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
	keypad->press_acc = kcalloc(pdata->num_row_gpios *
				    pdata->num_col_gpios,
				    sizeof(*keypad->press_acc), GFP_KERNEL);
	if (!keypad->raw_since || !keypad->press_acc) {
		err = -ENOMEM;
		goto err_free_mem;
	}
	matrix_keypad_parse_timing(&pdev->dev, keypad);
	err = matrix_keypad_map_rows(&pdev->dev, keypad);
	if (err)
		dev_warn(&pdev->dev, "row registers not mapped (%d): row_pull disabled\n",
			 err);

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
		kfree(keypad->press_acc);
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
	kfree(keypad->press_acc);
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
