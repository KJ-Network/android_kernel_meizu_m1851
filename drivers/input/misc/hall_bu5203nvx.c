// SPDX-License-Identifier: GPL-2.0
/*
 * BU5203NVX Hall sensor driver
 *
 * The sensor is connected as a GPIO switch and reports cover state through
 * the standard SW_LID input event.  The two legacy attributes are retained
 * for userspace which still consumes the vendor interface.
 */

#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

struct bu5203nvx_hall {
	struct device *dev;
	struct input_dev *input;
	struct work_struct work;
	int gpio;
	int irq;
	int state;
	int cover_control;
};

static void bu5203nvx_hall_work(struct work_struct *work)
{
	struct bu5203nvx_hall *hall =
		container_of(work, struct bu5203nvx_hall, work);
	int level;

	level = gpio_get_value_cansleep(hall->gpio);
	if (level) {
		hall->state = 3;
		input_report_switch(hall->input, SW_LID, 0);
	} else {
		hall->state = 2;
		input_report_switch(hall->input, SW_LID, 1);
	}

	input_sync(hall->input);
	enable_irq(hall->irq);
}

static irqreturn_t bu5203nvx_hall_irq(int irq, void *data)
{
	struct bu5203nvx_hall *hall = data;

	disable_irq_nosync(irq);
	schedule_work(&hall->work);
	return IRQ_HANDLED;
}

static ssize_t cover_control_show(struct device *dev,
					  struct device_attribute *attr, char *buf)
{
	struct bu5203nvx_hall *hall = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", hall->cover_control);
}

static ssize_t cover_control_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct bu5203nvx_hall *hall = dev_get_drvdata(dev);
	int value;

	if (kstrtoint(buf, 10, &value))
		return -EINVAL;

	hall->cover_control = value;
	return count;
}

static ssize_t key_hall_state_show(struct device *dev,
					   struct device_attribute *attr, char *buf)
{
	struct bu5203nvx_hall *hall = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", hall->state);
}

static DEVICE_ATTR(cover_control, 0644, cover_control_show,
		   cover_control_store);
static DEVICE_ATTR(key_hall_state, 0444, key_hall_state_show, NULL);

static struct attribute *bu5203nvx_hall_attrs[] = {
	&dev_attr_cover_control.attr,
	&dev_attr_key_hall_state.attr,
	NULL,
};

static const struct attribute_group bu5203nvx_hall_attr_group = {
	.attrs = bu5203nvx_hall_attrs,
};

static int bu5203nvx_hall_probe(struct platform_device *pdev)
{
	struct bu5203nvx_hall *hall;
	struct input_dev *input;
	int ret;

	hall = devm_kzalloc(&pdev->dev, sizeof(*hall), GFP_KERNEL);
	if (!hall)
		return -ENOMEM;

	hall->dev = &pdev->dev;
	hall->state = 2;
	platform_set_drvdata(pdev, hall);

	hall->gpio = of_get_named_gpio(pdev->dev.of_node, "irq_gpio", 0);
	if (!gpio_is_valid(hall->gpio))
		return hall->gpio < 0 ? hall->gpio : -EINVAL;

	ret = devm_gpio_request_one(&pdev->dev, hall->gpio, GPIOF_IN,
				   "bu5203nvx-hall-eint");
	if (ret)
		return ret;

	hall->irq = gpio_to_irq(hall->gpio);
	if (hall->irq < 0)
		return hall->irq;

	input = devm_input_allocate_device(&pdev->dev);
	if (!input)
		return -ENOMEM;

	hall->input = input;
	input->name = "bu5203nvx_hall";
	input->dev.parent = &pdev->dev;
	input_set_capability(input, EV_SW, SW_LID);
	ret = input_register_device(input);
	if (ret)
		return ret;

	INIT_WORK(&hall->work, bu5203nvx_hall_work);
	ret = request_threaded_irq(hall->irq, bu5203nvx_hall_irq, NULL,
				   IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
				   IRQF_ONESHOT | IRQF_NO_SUSPEND,
				   "bu5203nvx-hall-eint", hall);
	if (ret)
		return ret;

	ret = sysfs_create_group(&pdev->dev.kobj, &bu5203nvx_hall_attr_group);
	if (ret) {
		free_irq(hall->irq, hall);
		cancel_work_sync(&hall->work);
		return ret;
	}

	return 0;
}

static int bu5203nvx_hall_remove(struct platform_device *pdev)
{
	struct bu5203nvx_hall *hall = platform_get_drvdata(pdev);

	sysfs_remove_group(&pdev->dev.kobj, &bu5203nvx_hall_attr_group);
	free_irq(hall->irq, hall);
	cancel_work_sync(&hall->work);
	return 0;
}

static const struct of_device_id bu5203nvx_hall_of_match[] = {
	{ .compatible = "wind,bu5203nvx_hall" },
	{ }
};
MODULE_DEVICE_TABLE(of, bu5203nvx_hall_of_match);

static struct platform_driver bu5203nvx_hall_driver = {
	.probe = bu5203nvx_hall_probe,
	.remove = bu5203nvx_hall_remove,
	.driver = {
		.name = "bu5203nvx_hall",
		.of_match_table = bu5203nvx_hall_of_match,
	},
};
module_platform_driver(bu5203nvx_hall_driver);

MODULE_AUTHOR("Kopsources.ORG");
MODULE_DESCRIPTION("BU5203NVX Hall sensor driver");
MODULE_LICENSE("GPL v2");
