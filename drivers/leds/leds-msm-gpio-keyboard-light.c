/*
 * GPIO keypad backlight for the CAT S22 Flip.
 *
 * Copyright (c) 2013-2014, 2017, 2020, The Linux Foundation. All rights reserved.
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
 * Reimplementation of the "qcom,leds-gpio-keyboard_light" driver found in
 * the stock S22 v30 kernel, which is a trimmed copy of leds-msm-gpio-flash.c
 * (same strings and helper names). Behaviour recovered from its
 * disassembly: one GPIO from "qcom,keyboard_light", LED name from
 * "linux,name", trigger from "linux,default-trigger" (default "none"),
 * max_brightness 255, any brightness > 0 drives the GPIO high, registered
 * at late_initcall. Userspace writes /sys/class/leds/keyboard_light/brightness
 * (chowned to system by the vendor init.target.rc).
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/leds.h>
#include <linux/platform_device.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/printk.h>

#define LED_GPIO_KBD_DRIVER_NAME	"qcom,leds-gpio-keyboard_light"
#define LED_TRIGGER_DEFAULT		"none"

struct led_gpio_kbd_data {
	int gpio;
	int brightness;
	struct led_classdev cdev;
};

static void led_gpio_brightness_set(struct led_classdev *led_cdev,
				    enum led_brightness value)
{
	struct led_gpio_kbd_data *led =
		container_of(led_cdev, struct led_gpio_kbd_data, cdev);
	int rc;

	rc = gpio_direction_output(led->gpio, value > LED_OFF ? 1 : 0);
	if (rc) {
		pr_err("%s: Failed to set gpio %d\n", __func__, led->gpio);
		return;
	}
	led->brightness = value;
}

static enum led_brightness led_gpio_brightness_get(struct led_classdev *led_cdev)
{
	struct led_gpio_kbd_data *led =
		container_of(led_cdev, struct led_gpio_kbd_data, cdev);

	return led->brightness;
}

static int led_gpio_keyboard_light_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct led_gpio_kbd_data *led;
	const char *temp_str;
	int rc;

	led = devm_kzalloc(&pdev->dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	led->cdev.default_trigger = LED_TRIGGER_DEFAULT;
	rc = of_property_read_string(node, "linux,default-trigger", &temp_str);
	if (!rc)
		led->cdev.default_trigger = temp_str;

	led->gpio = of_get_named_gpio(node, "qcom,keyboard_light", 0);
	if (led->gpio < 0) {
		dev_err(&pdev->dev,
			"Looking up %s property in node %s failed. rc =  %d\n",
			"qcom,keyboard_light", node->full_name, led->gpio);
		return led->gpio;
	}

	rc = devm_gpio_request(&pdev->dev, led->gpio, "keyboard_light");
	if (rc) {
		dev_err(&pdev->dev, "%s: Failed to request gpio %d,rc = %d\n",
			__func__, led->gpio, rc);
		return rc;
	}

	rc = of_property_read_string(node, "linux,name", &led->cdev.name);
	if (rc) {
		dev_err(&pdev->dev, "%s: Failed to read linux name. rc = %d\n",
			__func__, rc);
		return rc;
	}

	platform_set_drvdata(pdev, led);
	led->cdev.max_brightness = LED_FULL;
	led->cdev.brightness_set = led_gpio_brightness_set;
	led->cdev.brightness_get = led_gpio_brightness_get;

	rc = led_classdev_register(&pdev->dev, &led->cdev);
	if (rc) {
		dev_err(&pdev->dev, "%s: Failed to register led dev. rc = %d\n",
			__func__, rc);
		return rc;
	}

	return 0;
}

static int led_gpio_keyboard_light_remove(struct platform_device *pdev)
{
	struct led_gpio_kbd_data *led = platform_get_drvdata(pdev);

	led_classdev_unregister(&led->cdev);
	return 0;
}

static const struct of_device_id led_gpio_keyboard_light_of_match[] = {
	{ .compatible = LED_GPIO_KBD_DRIVER_NAME, },
	{ },
};
MODULE_DEVICE_TABLE(of, led_gpio_keyboard_light_of_match);

static struct platform_driver led_gpio_keyboard_light_driver = {
	.probe = led_gpio_keyboard_light_probe,
	.remove = led_gpio_keyboard_light_remove,
	.driver = {
		.name = LED_GPIO_KBD_DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = led_gpio_keyboard_light_of_match,
	},
};

static int __init led_gpio_keyboard_light_init(void)
{
	return platform_driver_register(&led_gpio_keyboard_light_driver);
}

static void __exit led_gpio_keyboard_light_exit(void)
{
	platform_driver_unregister(&led_gpio_keyboard_light_driver);
}

late_initcall(led_gpio_keyboard_light_init);
module_exit(led_gpio_keyboard_light_exit);

MODULE_DESCRIPTION("GPIO keypad backlight (CAT S22 Flip)");
MODULE_LICENSE("GPL v2");
