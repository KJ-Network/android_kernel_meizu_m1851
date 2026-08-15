/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Kopsources.ORG
 *
 * Fingerprint sensor selector
 */
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

#include <linux/fingerprint_id.h>

struct fingerprint_id_data {
	struct regulator *vdd;
	int gpio;
	int value;
};

static DEFINE_MUTEX(fingerprint_id_lock);
static struct fingerprint_id_data *fingerprint_id_data;

int fingerprint_id_match(struct device *dev, enum fingerprint_sensor_id id)
{
	struct device_node *selector;
	int value;

	mutex_lock(&fingerprint_id_lock);
	if (fingerprint_id_data) {
		value = fingerprint_id_data->value;
		mutex_unlock(&fingerprint_id_lock);
		return value == id ? 0 : -ENODEV;
	}
	mutex_unlock(&fingerprint_id_lock);

	selector = of_find_compatible_node(NULL, NULL, "wind,fp_id");
	if (selector) {
		of_node_put(selector);
		return -EPROBE_DEFER;
	}

	/* A single-sensor board does not need a selector node. */
	return 0;
}
EXPORT_SYMBOL_GPL(fingerprint_id_match);

static int fingerprint_id_probe(struct platform_device *pdev)
{
	struct fingerprint_id_data *data;
	int error;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->vdd = devm_regulator_get_optional(&pdev->dev, "vdd_fp");
	if (IS_ERR(data->vdd)) {
		error = PTR_ERR(data->vdd);
		if (error != -ENODEV)
			return error;
		data->vdd = NULL;
	} else {
		if (regulator_count_voltages(data->vdd) > 0) {
			error = regulator_set_voltage(data->vdd, 2850000,
						      2850000);
			if (error)
				return error;
		}
		error = regulator_enable(data->vdd);
		if (error)
			return error;
	}

	data->gpio = of_get_named_gpio(pdev->dev.of_node,
				       "finger,gpio_id", 0);
	if (!gpio_is_valid(data->gpio)) {
		error = -EINVAL;
		goto err_supply;
	}
	error = devm_gpio_request_one(&pdev->dev, data->gpio, GPIOF_IN,
				      "finger_id");
	if (error)
		goto err_supply;
	data->value = gpio_get_value(data->gpio) ?
		FINGERPRINT_ID_ET512 : FINGERPRINT_ID_GOODIX;

	mutex_lock(&fingerprint_id_lock);
	fingerprint_id_data = data;
	mutex_unlock(&fingerprint_id_lock);
	platform_set_drvdata(pdev, data);
	dev_info(&pdev->dev, "fingerprint sensor id: %d\n", data->value);
	return 0;

err_supply:
	if (data->vdd)
		regulator_disable(data->vdd);
	return error;
}

static int fingerprint_id_remove(struct platform_device *pdev)
{
	struct fingerprint_id_data *data = platform_get_drvdata(pdev);

	mutex_lock(&fingerprint_id_lock);
	if (fingerprint_id_data == data)
		fingerprint_id_data = NULL;
	mutex_unlock(&fingerprint_id_lock);
	if (data && data->vdd)
		regulator_disable(data->vdd);
	return 0;
}

static const struct of_device_id fingerprint_id_match_table[] = {
	{ .compatible = "wind,fp_id" },
	{ }
};
MODULE_DEVICE_TABLE(of, fingerprint_id_match_table);

static struct platform_driver fingerprint_id_driver = {
	.probe = fingerprint_id_probe,
	.remove = fingerprint_id_remove,
	.driver = {
		.name = "finger_id",
		.of_match_table = fingerprint_id_match_table,
	},
};

module_platform_driver(fingerprint_id_driver);

MODULE_DESCRIPTION("Fingerprint sensor selector");
MODULE_LICENSE("GPL");
