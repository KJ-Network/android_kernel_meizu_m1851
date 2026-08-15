/*
 * platform indepent driver interface
 *
 * Coypritht (c) 2017 Goodix
 * Copyright (C) 2026 Kopsources.ORG
 */
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>

#include "gf_spi.h"

int gf_parse_dts(struct gf_device *data)
{
	struct device_node *np = data->pdev->dev.of_node;
	int ret;

	data->reset_gpio = of_get_named_gpio(np, "goodix,gpio_reset", 0);
	data->irq_gpio = of_get_named_gpio(np, "goodix,gpio_irq", 0);
	if (!gpio_is_valid(data->reset_gpio) || !gpio_is_valid(data->irq_gpio))
		return -EINVAL;
	ret = devm_gpio_request_one(&data->pdev->dev, data->reset_gpio,
				    GPIOF_OUT_INIT_HIGH, "goodix_reset");
	if (ret)
		return ret;
	ret = devm_gpio_request_one(&data->pdev->dev, data->irq_gpio,
				    GPIOF_IN, "goodix_irq");
	if (ret)
		return ret;
	return gf_irq_num(data);
}

void gf_cleanup(struct gf_device *data)
{
	if (data->wakeup) {
		wakeup_source_unregister(data->wakeup);
		data->wakeup = NULL;
	}
	if (data->irq_wake_enabled) {
		disable_irq_wake(data->irq);
		data->irq_wake_enabled = false;
	}
}

int gf_power_on(struct gf_device *data)
{
	data->device_available = true;
	return 0;
}

int gf_power_off(struct gf_device *data)
{
	data->device_available = false;
	return 0;
}

int gf_hw_reset(struct gf_device *data, unsigned int delay_ms)
{
	if (!data || !gpio_is_valid(data->reset_gpio))
		return -EINVAL;
	gpio_direction_output(data->reset_gpio, 1);
	gpio_set_value(data->reset_gpio, 0);
	usleep_range(2900, 3100);
	gpio_set_value(data->reset_gpio, 1);
	if (delay_ms)
		usleep_range(delay_ms * 1000, delay_ms * 1100);
	return 0;
}

int gf_irq_num(struct gf_device *data)
{
	data->irq = gpio_to_irq(data->irq_gpio);
	return data->irq < 0 ? data->irq : 0;
}
