// SPDX-License-Identifier: GPL-2.0
/*
 * Backlight black frame insertion (BFI) for Rockchip PWM backlights.
 *
 * Normally the backlight PWM runs a fast dimming carrier. With BFI on, it is
 * switched to a single lit pulse per panel frame: the picture is only seen for
 * part of each frame, which removes the sample-and-hold smear of moving content.
 *
 * Lock to the panel scan: the display controller calls gammaos_bfi_vblank() from
 * the frame start interrupt of the video port that drives the panel. Each frame a
 * high resolution timer then restarts the PWM counter at the chosen point, and the
 * pulse starts there. The PWM period is set a little LONGER than a frame, so the
 * counter is always restarted before it wraps and there is exactly one pulse per
 * frame. If frame interrupts stop, the PWM keeps running on its own slightly below
 * the frame rate, which is harmless.
 *
 * Where the pulse sits: the panel is written from top to bottom across the frame,
 * so any single pulse lights some rows while they are still changing. The RG DS
 * Plus panels update their rows later than the display controller sends them (a
 * 240 fps capture showed the bottom rows barely changed during a pulse placed at
 * the end of the frame), so the default pulse starts 1/16 of a frame after frame
 * start, the best overall compromise between the top and the bottom of the screen
 * found by eye on the four-box motion pattern. bfi_phase "auto" ends the pulse at
 * the next frame start instead.
 *
 * Brightness keeps working: the backlight level becomes the pulse length inside the
 * lit window, with a floor below which the backlight boost no longer lights.
 *
 * sysfs, on each backlight device that carries "gammaos,bfi-vp" in the device tree:
 *   bfi_dark    0 (off), 25, 50, 75: percent of each frame kept dark
 *   bfi_window  lit window in 1024ths of a frame (64..1024), turns BFI on
 *   bfi_phase   0..1023, the pulse start in 1024ths of a frame after frame start
 *               (default 64), or "auto" (pulse ends at the next frame start)
 *   bfi_floor   shortest pulse in 1024ths of a frame (0..1024)
 *   bfi_status  read only: state, counters and the programmed timing
 */
#include <linux/backlight.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gammaos_bfi.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pwm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

/* Rockchip PWM v1-v3 channel registers (one 0x10 block per channel) */
#define BFI_REG_PERIOD		0x04
#define BFI_REG_DUTY		0x08
#define BFI_REG_CTRL		0x0c
#define BFI_CTRL_ENABLE		BIT(0)
#define BFI_CTRL_LOCK		BIT(6)

#define BFI_FRAME_NS_DEFAULT	16715000ULL	/* 59.826 Hz */
#define BFI_FRAME_NS_MIN	8000000ULL	/* accept 40..125 Hz frame intervals */
#define BFI_FRAME_NS_MAX	25000000ULL
#define BFI_KICK_NOW_NS		20000		/* closer than this: restart at once */

struct gammaos_bfi {
	struct list_head node;
	struct backlight_device *bl;
	struct pwm_device *pwm;
	void __iomem *base;
	struct clk *pclk;
	u32 vp;

	struct mutex mlock;		/* settings and ownership changes */
	spinlock_t lock;		/* timing, shared with the interrupt paths */
	struct hrtimer timer;
	struct delayed_work vbl_work;

	/* settings */
	bool enabled;
	u32 window;			/* lit window, 1024ths of a frame */
	int phase;			/* -1 auto, else 1024ths of a frame */
	u32 floor;			/* shortest pulse, 1024ths of a frame */

	/* state */
	bool owning;			/* BFI drives the PWM registers */
	bool vbl_held;
	u32 level;			/* brightness 0..1024 */
	u32 saved_period, saved_duty, saved_ctrl;
	u64 counts_per_s;
	u64 frame_ns;
	u64 programmed_frame_ns;
	u32 period_cnt, duty_cnt;
	u64 delay_ns;
	ktime_t last_vs;
	u64 ticks, kicks;
};

static LIST_HEAD(bfi_list);
static DEFINE_SPINLOCK(bfi_list_lock);
static int (*bfi_vbl_get)(unsigned int vp);
static void (*bfi_vbl_put)(unsigned int vp);
static DEFINE_MUTEX(bfi_ctl_lock);

static u32 ns_to_cnt(struct gammaos_bfi *b, u64 ns)
{
	return (u32)div_u64(ns * b->counts_per_s, NSEC_PER_SEC);
}

static void bfi_write_timing(struct gammaos_bfi *b, u32 period, u32 duty)
{
	u32 ctrl = readl_relaxed(b->base + BFI_REG_CTRL);

	/* latch period and duty together, then release (10 PWM clocks apart) */
	writel_relaxed(ctrl | BFI_CTRL_LOCK, b->base + BFI_REG_CTRL);
	writel_relaxed(period, b->base + BFI_REG_PERIOD);
	writel_relaxed(duty, b->base + BFI_REG_DUTY);
	ndelay(1000);
	writel(ctrl & ~BFI_CTRL_LOCK, b->base + BFI_REG_CTRL);
}

/* restart the PWM counter: the lit part of the period starts now */
static void bfi_kick(struct gammaos_bfi *b)
{
	u32 ctrl = readl_relaxed(b->base + BFI_REG_CTRL);

	writel_relaxed(ctrl & ~BFI_CTRL_ENABLE, b->base + BFI_REG_CTRL);
	writel(ctrl | BFI_CTRL_ENABLE, b->base + BFI_REG_CTRL);
	b->kicks++;
}

/* work out the pulse for the current frame time and level; lock held */
static void bfi_program(struct gammaos_bfi *b)
{
	u64 frame = b->frame_ns;
	u64 win = div_u64(frame * b->window, 1024);
	u64 floor = div_u64(frame * b->floor, 1024);
	u64 pulse = div_u64(win * b->level, 1024);
	u64 period = frame + div_u64(frame, 16);	/* never wraps before the next restart */

	if (pulse < floor)
		pulse = floor;
	if (pulse > frame - div_u64(frame, 64))
		pulse = frame - div_u64(frame, 64);
	if (b->phase < 0)
		b->delay_ns = frame - pulse;
	else
		b->delay_ns = div_u64(frame * b->phase, 1024);
	b->period_cnt = ns_to_cnt(b, period);
	b->duty_cnt = ns_to_cnt(b, pulse);
	b->programmed_frame_ns = frame;
	bfi_write_timing(b, b->period_cnt, b->duty_cnt);
}

static enum hrtimer_restart bfi_timer_fn(struct hrtimer *t)
{
	struct gammaos_bfi *b = container_of(t, struct gammaos_bfi, timer);
	unsigned long flags;

	spin_lock_irqsave(&b->lock, flags);
	if (b->owning)
		bfi_kick(b);
	spin_unlock_irqrestore(&b->lock, flags);
	return HRTIMER_NORESTART;
}

void gammaos_bfi_vblank(unsigned int vp)
{
	struct gammaos_bfi *b;
	ktime_t now = ktime_get();
	unsigned long flags;

	spin_lock_irqsave(&bfi_list_lock, flags);
	list_for_each_entry(b, &bfi_list, node) {
		u64 dt;

		if (b->vp != vp)
			continue;
		spin_lock(&b->lock);
		dt = ktime_to_ns(ktime_sub(now, b->last_vs));
		b->last_vs = now;
		if (dt >= BFI_FRAME_NS_MIN && dt <= BFI_FRAME_NS_MAX)
			b->frame_ns = div_u64(b->frame_ns * 15 + dt, 16);
		b->ticks++;
		if (b->owning) {
			u64 pf = b->programmed_frame_ns;

			/* follow refresh changes (more than 0.2 %) without rewriting every frame */
			if (b->frame_ns > pf + div_u64(pf, 512) || b->frame_ns + div_u64(pf, 512) < pf)
				bfi_program(b);
			if (b->delay_ns < BFI_KICK_NOW_NS)
				bfi_kick(b);
			else
				hrtimer_start(&b->timer, ns_to_ktime(b->delay_ns),
					      HRTIMER_MODE_REL_HARD);
		}
		spin_unlock(&b->lock);
	}
	spin_unlock_irqrestore(&bfi_list_lock, flags);
}
EXPORT_SYMBOL_GPL(gammaos_bfi_vblank);

void gammaos_bfi_set_vblank_ctl(int (*get)(unsigned int vp), void (*put)(unsigned int vp))
{
	mutex_lock(&bfi_ctl_lock);
	bfi_vbl_get = get;
	bfi_vbl_put = put;
	mutex_unlock(&bfi_ctl_lock);
}
EXPORT_SYMBOL_GPL(gammaos_bfi_set_vblank_ctl);

/*
 * Keep the video port's frame interrupt running while BFI is on, so the lock holds
 * on a static screen. The port may not be enabled yet (panel off): retry later.
 */
static void bfi_vbl_hold(struct gammaos_bfi *b, bool hold)
{
	mutex_lock(&bfi_ctl_lock);
	if (hold && !b->vbl_held && bfi_vbl_get)
		b->vbl_held = bfi_vbl_get(b->vp) == 0;
	else if (!hold && b->vbl_held && bfi_vbl_put) {
		bfi_vbl_put(b->vp);
		b->vbl_held = false;
	}
	mutex_unlock(&bfi_ctl_lock);
	if (hold && !b->vbl_held)
		schedule_delayed_work(&b->vbl_work, msecs_to_jiffies(500));
}

static void bfi_vbl_work_fn(struct work_struct *w)
{
	struct gammaos_bfi *b = container_of(to_delayed_work(w), struct gammaos_bfi, vbl_work);

	mutex_lock(&b->mlock);
	if (b->enabled)
		bfi_vbl_hold(b, true);
	mutex_unlock(&b->mlock);
}

/* hand the PWM back exactly as it was; mlock held */
static void bfi_release(struct gammaos_bfi *b)
{
	unsigned long flags;

	if (!b->owning)
		return;
	spin_lock_irqsave(&b->lock, flags);
	b->owning = false;
	spin_unlock_irqrestore(&b->lock, flags);
	hrtimer_cancel(&b->timer);
	spin_lock_irqsave(&b->lock, flags);
	bfi_write_timing(b, b->saved_period, b->saved_duty);
	writel(b->saved_ctrl, b->base + BFI_REG_CTRL);
	spin_unlock_irqrestore(&b->lock, flags);
	clk_disable_unprepare(b->pclk);
}

/* take the PWM over; mlock held, the PWM is enabled by pwm_bl */
static int bfi_take(struct gammaos_bfi *b)
{
	struct pwm_state st;
	unsigned long flags;
	int ret;

	ret = clk_prepare_enable(b->pclk);
	if (ret)
		return ret;
	pwm_get_state(b->pwm, &st);
	spin_lock_irqsave(&b->lock, flags);
	b->saved_period = readl_relaxed(b->base + BFI_REG_PERIOD);
	b->saved_duty = readl_relaxed(b->base + BFI_REG_DUTY);
	b->saved_ctrl = readl_relaxed(b->base + BFI_REG_CTRL);
	/* the PWM's real count rate, from what the PWM driver programmed */
	if (st.period && b->saved_period)
		b->counts_per_s = div_u64((u64)b->saved_period * NSEC_PER_SEC, st.period);
	if (!b->counts_per_s) {
		spin_unlock_irqrestore(&b->lock, flags);
		clk_disable_unprepare(b->pclk);
		return -EINVAL;
	}
	b->owning = true;
	bfi_program(b);
	bfi_kick(b);
	spin_unlock_irqrestore(&b->lock, flags);
	return 0;
}

bool gammaos_bfi_enabled(struct gammaos_bfi *b)
{
	return b && READ_ONCE(b->enabled);
}
EXPORT_SYMBOL_GPL(gammaos_bfi_enabled);

void gammaos_bfi_set_level(struct gammaos_bfi *b, u32 level)
{
	unsigned long flags;

	if (!b)
		return;
	mutex_lock(&b->mlock);
	if (level > 1024)
		level = 1024;
	if (!level || !b->enabled) {
		bfi_release(b);
		b->level = level;
		mutex_unlock(&b->mlock);
		return;
	}
	b->level = level;
	if (!b->owning) {
		if (bfi_take(b))
			dev_warn(&b->bl->dev, "bfi: could not take over the PWM\n");
	} else {
		spin_lock_irqsave(&b->lock, flags);
		bfi_program(b);
		spin_unlock_irqrestore(&b->lock, flags);
	}
	mutex_unlock(&b->mlock);
	bfi_vbl_hold(b, true);
}
EXPORT_SYMBOL_GPL(gammaos_bfi_set_level);

/* ---- sysfs ---------------------------------------------------------------- */

static struct gammaos_bfi *bfi_from_dev(struct device *dev)
{
	struct backlight_device *bl = to_backlight_device(dev);
	struct gammaos_bfi *b, *found = NULL;
	unsigned long flags;

	spin_lock_irqsave(&bfi_list_lock, flags);
	list_for_each_entry(b, &bfi_list, node)
		if (b->bl == bl)
			found = b;
	spin_unlock_irqrestore(&bfi_list_lock, flags);
	return found;
}

/* apply a settings change: pwm_bl recomputes the level and calls set_level */
static void bfi_apply(struct gammaos_bfi *b, bool was_enabled)
{
	if (was_enabled && !b->enabled) {
		mutex_lock(&b->mlock);
		bfi_release(b);
		mutex_unlock(&b->mlock);
		cancel_delayed_work_sync(&b->vbl_work);
		bfi_vbl_hold(b, false);
	}
	backlight_update_status(b->bl);
}

static ssize_t bfi_dark_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct gammaos_bfi *b = bfi_from_dev(dev);

	if (!b)
		return -ENODEV;
	return sysfs_emit(buf, "%u\n", b->enabled ? 100 - DIV_ROUND_CLOSEST(b->window * 100, 1024) : 0);
}

static ssize_t bfi_dark_store(struct device *dev, struct device_attribute *a,
			      const char *buf, size_t n)
{
	struct gammaos_bfi *b = bfi_from_dev(dev);
	bool was;
	u32 v;

	if (!b)
		return -ENODEV;
	if (kstrtou32(buf, 0, &v) || v > 94)
		return -EINVAL;
	mutex_lock(&b->mlock);
	was = b->enabled;
	if (v) {
		b->window = 1024 - DIV_ROUND_CLOSEST(v * 1024, 100);
		WRITE_ONCE(b->enabled, true);
	} else {
		WRITE_ONCE(b->enabled, false);
	}
	mutex_unlock(&b->mlock);
	bfi_apply(b, was);
	return n;
}
static DEVICE_ATTR_RW(bfi_dark);

static ssize_t bfi_window_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct gammaos_bfi *b = bfi_from_dev(dev);

	return b ? sysfs_emit(buf, "%u\n", b->window) : -ENODEV;
}

static ssize_t bfi_window_store(struct device *dev, struct device_attribute *a,
				const char *buf, size_t n)
{
	struct gammaos_bfi *b = bfi_from_dev(dev);
	bool was;
	u32 v;

	if (!b)
		return -ENODEV;
	if (kstrtou32(buf, 0, &v) || v < 64 || v > 1024)
		return -EINVAL;
	mutex_lock(&b->mlock);
	was = b->enabled;
	b->window = v;
	WRITE_ONCE(b->enabled, true);
	mutex_unlock(&b->mlock);
	bfi_apply(b, was);
	return n;
}
static DEVICE_ATTR_RW(bfi_window);

static ssize_t bfi_phase_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct gammaos_bfi *b = bfi_from_dev(dev);

	if (!b)
		return -ENODEV;
	return b->phase < 0 ? sysfs_emit(buf, "auto\n") : sysfs_emit(buf, "%d\n", b->phase);
}

static ssize_t bfi_phase_store(struct device *dev, struct device_attribute *a,
			       const char *buf, size_t n)
{
	struct gammaos_bfi *b = bfi_from_dev(dev);
	int v;

	if (!b)
		return -ENODEV;
	if (sysfs_streq(buf, "auto"))
		v = -1;
	else if (kstrtoint(buf, 0, &v) || v < -1 || v > 1023)
		return -EINVAL;
	mutex_lock(&b->mlock);
	b->phase = v;
	mutex_unlock(&b->mlock);
	bfi_apply(b, b->enabled);
	return n;
}
static DEVICE_ATTR_RW(bfi_phase);

static ssize_t bfi_floor_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct gammaos_bfi *b = bfi_from_dev(dev);

	return b ? sysfs_emit(buf, "%u\n", b->floor) : -ENODEV;
}

static ssize_t bfi_floor_store(struct device *dev, struct device_attribute *a,
			       const char *buf, size_t n)
{
	struct gammaos_bfi *b = bfi_from_dev(dev);
	u32 v;

	if (!b)
		return -ENODEV;
	if (kstrtou32(buf, 0, &v) || v > 1024)
		return -EINVAL;
	mutex_lock(&b->mlock);
	b->floor = v;
	mutex_unlock(&b->mlock);
	bfi_apply(b, b->enabled);
	return n;
}
static DEVICE_ATTR_RW(bfi_floor);

static ssize_t bfi_status_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct gammaos_bfi *b = bfi_from_dev(dev);
	unsigned long flags;
	ssize_t n;

	if (!b)
		return -ENODEV;
	spin_lock_irqsave(&b->lock, flags);
	n = sysfs_emit(buf,
		       "enabled=%d owning=%d vblank_held=%d vp=%u level=%u window=%u phase=%d floor=%u\n"
		       "frame_ns=%llu delay_ns=%llu period_cnt=%u duty_cnt=%u counts_per_s=%llu\n"
		       "ticks=%llu kicks=%llu\n",
		       b->enabled, b->owning, b->vbl_held, b->vp, b->level, b->window, b->phase,
		       b->floor, b->frame_ns, b->delay_ns, b->period_cnt, b->duty_cnt,
		       b->counts_per_s, b->ticks, b->kicks);
	spin_unlock_irqrestore(&b->lock, flags);
	return n;
}
static DEVICE_ATTR_RO(bfi_status);

static struct attribute *bfi_attrs[] = {
	&dev_attr_bfi_dark.attr,
	&dev_attr_bfi_window.attr,
	&dev_attr_bfi_phase.attr,
	&dev_attr_bfi_floor.attr,
	&dev_attr_bfi_status.attr,
	NULL,
};

static const struct attribute_group bfi_group = {
	.attrs = bfi_attrs,
};

/* ---- attach / detach ------------------------------------------------------ */

struct gammaos_bfi *gammaos_bfi_attach(struct backlight_device *bl, struct pwm_device *pwm, u32 vp)
{
	struct device *chipdev = pwm && pwm->chip ? pwm->chip->dev : NULL;
	struct gammaos_bfi *b;
	struct resource res;
	unsigned long flags;

	if (!chipdev || !chipdev->of_node)
		return NULL;
	if (of_address_to_resource(chipdev->of_node, 0, &res) || resource_size(&res) < 0x10) {
		dev_warn(&bl->dev, "bfi: no PWM registers\n");
		return NULL;
	}
	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return NULL;
	b->base = ioremap(res.start, resource_size(&res));
	b->pclk = clk_get(chipdev, "pclk");
	if (!b->base || IS_ERR(b->pclk)) {
		dev_warn(&bl->dev, "bfi: cannot map the PWM or get its bus clock\n");
		if (b->base)
			iounmap(b->base);
		if (!IS_ERR_OR_NULL(b->pclk))
			clk_put(b->pclk);
		kfree(b);
		return NULL;
	}
	b->bl = bl;
	b->pwm = pwm;
	b->vp = vp;
	b->window = 512;
	b->phase = 64;
	b->floor = 192;
	b->frame_ns = BFI_FRAME_NS_DEFAULT;
	mutex_init(&b->mlock);
	spin_lock_init(&b->lock);
	hrtimer_init(&b->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
	b->timer.function = bfi_timer_fn;
	INIT_DELAYED_WORK(&b->vbl_work, bfi_vbl_work_fn);

	spin_lock_irqsave(&bfi_list_lock, flags);
	list_add_tail(&b->node, &bfi_list);
	spin_unlock_irqrestore(&bfi_list_lock, flags);

	if (sysfs_create_group(&bl->dev.kobj, &bfi_group))
		dev_warn(&bl->dev, "bfi: no sysfs controls\n");
	dev_info(&bl->dev, "bfi: ready on video port %u (PWM at %pa)\n", vp, &res.start);
	return b;
}
EXPORT_SYMBOL_GPL(gammaos_bfi_attach);

void gammaos_bfi_detach(struct gammaos_bfi *b)
{
	unsigned long flags;

	if (!b)
		return;
	sysfs_remove_group(&b->bl->dev.kobj, &bfi_group);
	mutex_lock(&b->mlock);
	WRITE_ONCE(b->enabled, false);
	bfi_release(b);
	mutex_unlock(&b->mlock);
	cancel_delayed_work_sync(&b->vbl_work);
	bfi_vbl_hold(b, false);
	spin_lock_irqsave(&bfi_list_lock, flags);
	list_del(&b->node);
	spin_unlock_irqrestore(&bfi_list_lock, flags);
	hrtimer_cancel(&b->timer);
	clk_put(b->pclk);
	iounmap(b->base);
	kfree(b);
}
EXPORT_SYMBOL_GPL(gammaos_bfi_detach);
