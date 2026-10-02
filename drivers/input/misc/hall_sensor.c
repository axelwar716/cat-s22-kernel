/*
 * Hall effect lid sensor for the CAT S22 Flip.
 *
 * Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Based on the Qualcomm hall_sensor.c ("hall-switch") as published in
 * HMD-OSS-Archive/android_kernel_nokia_6300_4g (461fc3097cf7) and
 * TINNO-RD/c330_kernel (d5d4a325dfe1). Behaviour aligned with the driver in
 * the stock S22 v30 kernel (4.9.227-perf), recovered from its disassembly:
 *
 *  - DT: "linux,gpio-int" (+flags), "linux,wakeup", "linux,min-uv",
 *    "linux,max-uv", "vddio-supply"; compatible "hall-switch";
 *  - state = (gpio level != 0) ^ !(flags & OF_GPIO_ACTIVE_LOW); on the S22
 *    (flags = ACTIVE_LOW) this is the raw level: 1 = far (lid open),
 *    0 = near (lid closed). The stock framework treats "1" as not covered;
 *  - state published through the Android switch device "hall_switch"
 *    (class "switch_hall"), updated about 300 ms after the edge;
 *  - input device "hall_sensor", phys "/dev/input/hall_dev", EV_SW/SW_LID.
 *
 * Difference from stock: the stock driver advertises SW_LID but never
 * reports it (it emits KEY_SLEEP/KEY_WAKEUP, which the input core drops
 * because EV_KEY is not set), so SW_LID stays 0 forever. This driver
 * reports SW_LID on every change. By default SW_LID mirrors the switch
 * state (1 = open, 0 = closed): with the lid closed it reads 0 exactly as
 * on stock, and LineageOS' frameworks-native-hall-polarity.patch, which
 * inverts SW_LID for "hall_sensor", turns it into the right lid state.
 * Set CONFIG_SENSORS_HALL_SW_LID_LINUX_POLARITY (or the sw_lid_linux_polarity
 * parameter) to report the standard Linux meaning (1 = closed) instead,
 * which is what an unpatched framework expects.
 */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/pm.h>
#include <linux/pm_wakeup.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/regulator/consumer.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/switch.h>

#define LID_DEV_NAME		"hall_sensor"
#define HALL_INPUT		"/dev/input/hall_dev"
#define HALL_SWITCH_NAME	"hall_switch"
#define HALL_DEBOUNCE_MS	300
#define HALL_WAKEUP_MS		(HALL_DEBOUNCE_MS + 700)

static bool sw_lid_linux_polarity =
	IS_ENABLED(CONFIG_SENSORS_HALL_SW_LID_LINUX_POLARITY);
module_param(sw_lid_linux_polarity, bool, 0444);
MODULE_PARM_DESC(sw_lid_linux_polarity,
		 "Report SW_LID=1 when closed (Linux convention) instead of the S22 stock-compatible 0");

struct hall_data {
	int gpio;		/* device use gpio number */
	int irq;		/* device request irq number */
	int active_low;		/* stock semantics: !(flags & OF_GPIO_ACTIVE_LOW) */
	bool wakeup;		/* device can wakeup system or not */
	struct device *dev;
	struct input_dev *hall_dev;
	struct regulator *vddio;
	u32 min_uv;		/* device allow minimum voltage */
	u32 max_uv;		/* device allow max voltage */
	struct switch_dev sdev;
	struct workqueue_struct *wq;
	struct delayed_work work;
	struct mutex lock;
	int state;		/* 1 = far (open), 0 = near (closed) */
};

/*
 * Notification hook for the SPI cover panel driver. The stock kernel calls
 * mdss_spi_get_hall_state(state) from its work function; the strong
 * definition lives in the OEM mdss_spi_panel code, which is not part of
 * this tree yet, so a no-op weak default is provided.
 */
void mdss_spi_get_hall_state(int state);
void __weak mdss_spi_get_hall_state(int state)
{
}

static int hall_read_state(struct hall_data *data)
{
	return (gpio_get_value_cansleep(data->gpio) ? 1 : 0) ^ data->active_low;
}

static void hall_report_lid(struct hall_data *data, int state)
{
	/* state 1 = open; Linux SW_LID 1 = closed */
	int sw = sw_lid_linux_polarity ? !state : state;

	input_report_switch(data->hall_dev, SW_LID, sw);
	input_sync(data->hall_dev);
}

static void hall_work_func(struct work_struct *work)
{
	struct hall_data *data =
		container_of(to_delayed_work(work), struct hall_data, work);
	int state;

	mutex_lock(&data->lock);
	state = hall_read_state(data);
	dev_dbg(data->dev, "hall gpio value = %d, %s\n",
		gpio_get_value_cansleep(data->gpio), state ? "far" : "near");

	if (state != data->state) {
		data->state = state;
		switch_set_state(&data->sdev, state);
		hall_report_lid(data, state);
		dev_info(data->dev, "hall change: %s\n", state ? "open" : "closed");
	}
	mutex_unlock(&data->lock);

	mdss_spi_get_hall_state(state);
}

static irqreturn_t hall_interrupt_handler(int irq, void *dev)
{
	struct hall_data *data = dev;

	pm_wakeup_event(data->dev, HALL_WAKEUP_MS);
	mod_delayed_work(data->wq, &data->work,
			 msecs_to_jiffies(HALL_DEBOUNCE_MS));

	return IRQ_HANDLED;
}

static int hall_input_init(struct platform_device *pdev,
		struct hall_data *data)
{
	int err;

	data->hall_dev = devm_input_allocate_device(&pdev->dev);
	if (!data->hall_dev) {
		dev_err(&pdev->dev, "input device allocation failed\n");
		return -EINVAL;
	}
	data->hall_dev->name = LID_DEV_NAME;
	data->hall_dev->phys = HALL_INPUT;
	__set_bit(EV_SW, data->hall_dev->evbit);
	__set_bit(SW_LID, data->hall_dev->swbit);

	err = input_register_device(data->hall_dev);
	if (err < 0) {
		dev_err(&pdev->dev, "unable to register input device %s\n",
				LID_DEV_NAME);
		return err;
	}

	return 0;
}

static int hall_config_regulator(struct platform_device *dev, bool on)
{
	struct hall_data *data = dev_get_drvdata(&dev->dev);
	int rc = 0;

	if (on) {
		data->vddio = devm_regulator_get(&dev->dev, "vddio");
		if (IS_ERR(data->vddio)) {
			rc = PTR_ERR(data->vddio);
			dev_err(&dev->dev, "Regulator vddio get failed rc=%d\n",
					rc);
			data->vddio = NULL;
			return rc;
		}

		if (regulator_count_voltages(data->vddio) > 0) {
			rc = regulator_set_voltage(
					data->vddio,
					data->min_uv,
					data->max_uv);
			if (rc) {
				dev_err(&dev->dev, "Regulator vddio Set voltage failed rc=%d\n",
						rc);
				goto deinit_vregs;
			}
		}
		return rc;
	}

deinit_vregs:
	if (data->vddio && regulator_count_voltages(data->vddio) > 0)
		regulator_set_voltage(data->vddio, 0, data->max_uv);

	return rc;
}

static int hall_set_regulator(struct platform_device *dev, bool on)
{
	struct hall_data *data = dev_get_drvdata(&dev->dev);
	int rc = 0;

	if (IS_ERR_OR_NULL(data->vddio))
		return 0;

	if (on) {
		rc = regulator_enable(data->vddio);
		if (rc)
			dev_err(&dev->dev, "Enable regulator vddio failed rc=%d\n",
				rc);
		return rc;
	}

	rc = regulator_disable(data->vddio);
	if (rc)
		dev_err(&dev->dev, "Disable regulator vddio failed rc=%d\n", rc);
	return 0;
}

#ifdef CONFIG_OF
static int hall_parse_dt(struct device *dev, struct hall_data *data)
{
	enum of_gpio_flags flags;
	u32 tempval;
	int rc;
	struct device_node *np = dev->of_node;

	data->gpio = of_get_named_gpio_flags(np, "linux,gpio-int", 0, &flags);
	if (!gpio_is_valid(data->gpio)) {
		dev_err(dev, "hall gpio is not valid\n");
		return -EINVAL;
	}
	data->active_low = (flags & OF_GPIO_ACTIVE_LOW) ? 0 : 1;

	data->wakeup = of_property_read_bool(np, "linux,wakeup");

	rc = of_property_read_u32(np, "linux,max-uv", &tempval);
	if (rc) {
		dev_err(dev, "unable to read max-uv\n");
		return -EINVAL;
	}
	data->max_uv = tempval;

	rc = of_property_read_u32(np, "linux,min-uv", &tempval);
	if (rc) {
		dev_err(dev, "unable to read min-uv\n");
		return -EINVAL;
	}
	data->min_uv = tempval;

	return 0;
}
#else
static int hall_parse_dt(struct device *dev, struct hall_data *data)
{
	return -EINVAL;
}
#endif

static int hall_driver_probe(struct platform_device *dev)
{
	struct hall_data *data;
	int err;

	data = devm_kzalloc(&dev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	dev_set_drvdata(&dev->dev, data);
	data->dev = &dev->dev;
	mutex_init(&data->lock);

	if (!dev->dev.of_node) {
		dev_err(&dev->dev, "No valid platform data.\n");
		return -ENODEV;
	}

	err = hall_parse_dt(&dev->dev, data);
	if (err < 0) {
		dev_err(&dev->dev, "Failed to parse device tree\n");
		return err;
	}

	err = hall_input_init(dev, data);
	if (err < 0) {
		dev_err(&dev->dev, "input init failed\n");
		return err;
	}

	err = gpio_request_one(data->gpio, GPIOF_DIR_IN, "hall_sensor_irq");
	if (err) {
		dev_err(&dev->dev, "unable to request gpio %d\n", data->gpio);
		return err;
	}

	data->wq = create_singlethread_workqueue("hall_wq");
	if (!data->wq) {
		err = -ENOMEM;
		goto free_gpio;
	}
	INIT_DELAYED_WORK(&data->work, hall_work_func);

	err = hall_config_regulator(dev, true);
	if (err < 0) {
		dev_err(&dev->dev, "Configure power failed: %d\n", err);
		goto free_wq;
	}

	err = hall_set_regulator(dev, true);
	if (err < 0) {
		dev_err(&dev->dev, "power on failed: %d\n", err);
		goto err_regulator_init;
	}

	data->state = hall_read_state(data);
	dev_info(&dev->dev, "hall_state is %d (%s)\n", data->state,
		 data->state ? "open" : "closed");

	data->sdev.name = HALL_SWITCH_NAME;
	err = switch_dev_register(&data->sdev);
	if (err < 0) {
		dev_err(&dev->dev, "hall switch registeration failed: %d\n", err);
		goto err_regulator_on;
	}
	/* stock sets the initial value without emitting a uevent */
	data->sdev.state = data->state;

	hall_report_lid(data, data->state);
	mdss_spi_get_hall_state(data->state);

	data->irq = gpio_to_irq(data->gpio);
	err = devm_request_threaded_irq(&dev->dev, data->irq, NULL,
			hall_interrupt_handler,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			LID_DEV_NAME, data);
	if (err < 0) {
		dev_err(&dev->dev, "request irq failed : %d\n", data->irq);
		goto err_switch;
	}

	device_init_wakeup(&dev->dev, data->wakeup);
	enable_irq_wake(data->irq);

	return 0;

err_switch:
	switch_dev_unregister(&data->sdev);
err_regulator_on:
	hall_set_regulator(dev, false);
err_regulator_init:
	hall_config_regulator(dev, false);
free_wq:
	destroy_workqueue(data->wq);
free_gpio:
	gpio_free(data->gpio);
	return err;
}

static int hall_driver_remove(struct platform_device *dev)
{
	struct hall_data *data = dev_get_drvdata(&dev->dev);

	disable_irq_wake(data->irq);
	device_init_wakeup(&dev->dev, 0);
	devm_free_irq(&dev->dev, data->irq, data);
	cancel_delayed_work_sync(&data->work);
	destroy_workqueue(data->wq);
	switch_dev_unregister(&data->sdev);
	gpio_free(data->gpio);
	hall_set_regulator(dev, false);
	hall_config_regulator(dev, false);

	return 0;
}

static const struct platform_device_id hall_id[] = {
	{LID_DEV_NAME, 0 },
	{ },
};
MODULE_DEVICE_TABLE(platform, hall_id);

#ifdef CONFIG_OF
static const struct of_device_id hall_match_table[] = {
	{.compatible = "hall-switch", },
	{ },
};
MODULE_DEVICE_TABLE(of, hall_match_table);
#endif

static struct platform_driver hall_driver = {
	.driver = {
		.name = LID_DEV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(hall_match_table),
	},
	.probe = hall_driver_probe,
	.remove = hall_driver_remove,
	.id_table = hall_id,
};

module_platform_driver(hall_driver);

MODULE_DESCRIPTION("Hall sensor driver (CAT S22 Flip)");
MODULE_LICENSE("GPL v2");
