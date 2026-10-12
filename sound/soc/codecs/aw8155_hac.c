// SPDX-License-Identifier: GPL-2.0
/*
 * AWINIC AW8155 class-K amplifier used as hearing-aid coil (HAC) driver
 * on the CAT S22 Flip.
 *
 * The amplifier has a single EN pin. Driving it low for more than 1 ms
 * shuts the amplifier down; a train of N rising edges (pulse width
 * 0.75..10 us) selects operating mode N. The vendor machine driver
 * (audio_machine_sdm450.ko, "AW8155 HAC" mixer) calls aw8155_set_en().
 *
 * DT binding (as shipped in the S22 v30 dtbo):
 *	aw8155 {
 *		compatible = "qcom,aw8155";
 *		aw8155,en-gpio = <&tlmm 38 0>;
 *		aw8155,mode = <1>;	(optional, 1..4, default 1)
 *	};
 *
 * Copyright (C) 2026 cat-s22
 */
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <sound/aw8155.h>

#define AW8155_MODE_MAX		4
#define AW8155_PULSE_US		2
#define AW8155_SHUTDOWN_US	1500

struct aw8155 {
	struct device *dev;
	int en_gpio;
	u32 mode;
	bool enabled;
	struct mutex lock;
	spinlock_t pulse_lock;
};

static struct aw8155 *aw8155_dev;
static DEFINE_MUTEX(aw8155_dev_lock);

static void aw8155_power_off(struct aw8155 *aw)
{
	gpio_set_value_cansleep(aw->en_gpio, 0);
	usleep_range(AW8155_SHUTDOWN_US, AW8155_SHUTDOWN_US + 100);
}

static void aw8155_power_on(struct aw8155 *aw)
{
	unsigned long flags;
	u32 i;

	/* the pulse train must not be stretched by preemption or IRQs */
	spin_lock_irqsave(&aw->pulse_lock, flags);
	for (i = 0; i < aw->mode; i++) {
		gpio_set_value(aw->en_gpio, 0);
		udelay(AW8155_PULSE_US);
		gpio_set_value(aw->en_gpio, 1);
		udelay(AW8155_PULSE_US);
	}
	spin_unlock_irqrestore(&aw->pulse_lock, flags);
}

/**
 * aw8155_set_en() - switch the HAC amplifier on or off
 * @en: true to enable in the configured mode, false to shut down
 *
 * Return: 0 on success, -ENODEV if no amplifier has been probed.
 */
int aw8155_set_en(bool en)
{
	struct aw8155 *aw;

	mutex_lock(&aw8155_dev_lock);
	aw = aw8155_dev;
	if (!aw) {
		mutex_unlock(&aw8155_dev_lock);
		pr_err("%s: AW8155 not probed\n", __func__);
		return -ENODEV;
	}

	mutex_lock(&aw->lock);
	/* always pass through shutdown so a mode change is latched */
	aw8155_power_off(aw);
	if (en)
		aw8155_power_on(aw);
	aw->enabled = en;
	mutex_unlock(&aw->lock);
	mutex_unlock(&aw8155_dev_lock);

	dev_info(aw->dev, "%s: %s (mode %u)\n", __func__,
		 en ? "on" : "off", aw->mode);
	return 0;
}
EXPORT_SYMBOL(aw8155_set_en);

static int aw8155_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct aw8155 *aw;
	int ret;

	aw = devm_kzalloc(&pdev->dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;

	aw->dev = &pdev->dev;
	mutex_init(&aw->lock);
	spin_lock_init(&aw->pulse_lock);

	aw->en_gpio = of_get_named_gpio(np, "aw8155,en-gpio", 0);
	if (!gpio_is_valid(aw->en_gpio)) {
		dev_err(&pdev->dev, "%s: invalid aw8155,en-gpio: %d\n",
			__func__, aw->en_gpio);
		return aw->en_gpio == -EPROBE_DEFER ? -EPROBE_DEFER : -EINVAL;
	}

	if (of_property_read_u32(np, "aw8155,mode", &aw->mode) ||
	    aw->mode < 1 || aw->mode > AW8155_MODE_MAX)
		aw->mode = 1;

	ret = devm_gpio_request_one(&pdev->dev, aw->en_gpio,
				    GPIOF_OUT_INIT_LOW, "aw8155_en");
	if (ret) {
		dev_err(&pdev->dev, "%s: gpio %d request failed: %d\n",
			__func__, aw->en_gpio, ret);
		return ret;
	}

	platform_set_drvdata(pdev, aw);
	mutex_lock(&aw8155_dev_lock);
	aw8155_dev = aw;
	mutex_unlock(&aw8155_dev_lock);

	dev_info(&pdev->dev, "%s: en-gpio %d, mode %u\n", __func__,
		 aw->en_gpio, aw->mode);
	return 0;
}

static int aw8155_remove(struct platform_device *pdev)
{
	struct aw8155 *aw = platform_get_drvdata(pdev);

	mutex_lock(&aw8155_dev_lock);
	aw8155_dev = NULL;
	mutex_unlock(&aw8155_dev_lock);
	gpio_set_value_cansleep(aw->en_gpio, 0);
	return 0;
}

static void aw8155_shutdown(struct platform_device *pdev)
{
	struct aw8155 *aw = platform_get_drvdata(pdev);

	if (aw)
		gpio_set_value_cansleep(aw->en_gpio, 0);
}

static const struct of_device_id aw8155_of_match[] = {
	{ .compatible = "qcom,aw8155" },
	{ .compatible = "awinic,aw8155" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw8155_of_match);

static struct platform_driver aw8155_driver = {
	.driver = {
		.name = "aw8155",
		.of_match_table = aw8155_of_match,
	},
	.probe = aw8155_probe,
	.remove = aw8155_remove,
	.shutdown = aw8155_shutdown,
};
module_platform_driver(aw8155_driver);

MODULE_DESCRIPTION("AWINIC AW8155 HAC amplifier enable control");
MODULE_LICENSE("GPL v2");
