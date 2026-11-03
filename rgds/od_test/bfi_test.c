// SPDX-License-Identifier: GPL-2.0
/*
 * bfi_test: one-off BFI feasibility test for the RG DS Plus backlight.
 *
 * Switches one backlight PWM channel from its 40 kHz dimming carrier to a period of
 * about one panel frame, so a person can see whether the backlight boost strobes
 * (BFI possible) or smooths the slow square wave into a steady level (BFI impossible).
 * Nothing is locked to the panel scan; this only answers the boost question.
 *
 * The channel's bus clock is held enabled for every register access (the PWM driver
 * gates it between accesses, and touching the block with it off hangs the bus).
 * The original period, duty and control values are restored on unload.
 *
 *   insmod bfi_test.ko dev=fdd70010.pwm period=200580 duty=100290
 *   rmmod bfi_test
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>

#define PWM_PERIOD	0x04
#define PWM_DUTY	0x08
#define PWM_CTRL	0x0c
#define PWM_LOCK_EN	BIT(6)

static char *dev = "fdd70010.pwm";
module_param(dev, charp, 0444);
static uint period = 200580;	/* 12 MHz clock: 16.715 ms, one 59.826 Hz frame */
module_param(period, uint, 0444);
static uint duty = 100290;	/* half the period lit */
module_param(duty, uint, 0444);

static struct device *pdev_dev;
static struct clk *pclk;
static void __iomem *base;
static u32 saved_period, saved_duty, saved_ctrl;

static void pwm_write(u32 p, u32 d)
{
	u32 ctrl = readl(base + PWM_CTRL);

	/* latch period and duty together so the output never sees a half update */
	writel(ctrl | PWM_LOCK_EN, base + PWM_CTRL);
	writel(p, base + PWM_PERIOD);
	writel(d, base + PWM_DUTY);
	writel(ctrl & ~PWM_LOCK_EN, base + PWM_CTRL);
}

static int __init bfi_test_init(void)
{
	struct resource res;
	int ret;

	pdev_dev = bus_find_device_by_name(&platform_bus_type, NULL, dev);
	if (!pdev_dev) {
		pr_err("bfi_test: no device %s\n", dev);
		return -ENODEV;
	}
	if (period < 1000 || duty > period) {
		ret = -EINVAL;
		goto put_dev;
	}
	ret = of_address_to_resource(pdev_dev->of_node, 0, &res);
	if (ret)
		goto put_dev;
	pclk = clk_get(pdev_dev, "pclk");
	if (IS_ERR(pclk)) {
		ret = PTR_ERR(pclk);
		goto put_dev;
	}
	ret = clk_prepare_enable(pclk);
	if (ret)
		goto put_clk;
	base = ioremap(res.start, resource_size(&res));
	if (!base) {
		ret = -ENOMEM;
		goto off_clk;
	}
	saved_period = readl(base + PWM_PERIOD);
	saved_duty = readl(base + PWM_DUTY);
	saved_ctrl = readl(base + PWM_CTRL);
	pr_info("bfi_test: %s at %pa was period=%u duty=%u ctrl=%08x, now period=%u duty=%u\n",
		dev, &res.start, saved_period, saved_duty, saved_ctrl, period, duty);
	pwm_write(period, duty);
	return 0;

off_clk:
	clk_disable_unprepare(pclk);
put_clk:
	clk_put(pclk);
put_dev:
	put_device(pdev_dev);
	return ret;
}

static void __exit bfi_test_exit(void)
{
	pwm_write(saved_period, saved_duty);
	writel(saved_ctrl, base + PWM_CTRL);
	pr_info("bfi_test: %s restored period=%u duty=%u\n", dev, saved_period, saved_duty);
	iounmap(base);
	clk_disable_unprepare(pclk);
	clk_put(pclk);
	put_device(pdev_dev);
}

module_init(bfi_test_init);
module_exit(bfi_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RG DS Plus backlight BFI feasibility test");
