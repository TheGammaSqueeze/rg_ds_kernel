/*----------------------------------------------------------------------------*/

/*
 * Copyright (c) 2008-2021 Anbernic 
 */

/*
 * Single SARADC dual-joystick gamepad driver
 */

/*----------------------------------------------------------------------------*/
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio_keys.h>
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>
#include <linux/property.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>

/*----------------------------------------------------------------------------*/
#define DRV_NAME "retrogame_joypad"
#define __LEFT_JOYSTICK_INVERT__
#define __MURMUR__
/*----------------------------------------------------------------------------*/
#define	ADC_MAX_VOLTAGE		1800
#define	ADC_DATA_TUNING(x, p)	((x * p) / 100)
#define	ADC_TUNING_DEFAULT	180
/*
 * Analog model, reverse-engineered 1:1 from the stock RG DS 6.1.141 kernel
 * (joypad_adc_read / joypad_adc_check).  The SARADC is read in the PROCESSED
 * (millivolt) domain, 0..1800 mV mapped onto 0..0x8000 via (mV << 15) / 1800;
 * rest is ~900 mV so a centred axis sits at ~0x4000.  Each axis is then
 * centre-subtracted against its rest calibration, 4-sample averaged, run
 * through a radial rest deadzone (snap to exactly 0), amplified by a fixed
 * global gain and snapped to the endpoint once it passes 0.98 of full scale.
 * This overdrive+snap is what guarantees FULL TRAVEL, and processing every
 * axis independently is what avoids the cardinal-snap the old fixed shift had.
 */
#define	ADC_ABS_RANGE		0x4000
/* stock jokstick_cal_scale = 3000 (x3.0 gain), applied as (val * SCALE)/1000 */
#define	JOY_CAL_SCALE		3000
/* stock jokstick_cal_deadzone = 1000 (radial rest deadzone, in report units) */
#define	JOY_CAL_DEADZONE	1000
/* stock 4-deep moving average */
#define	JOY_AVG_DEPTH		4
/*
 * Full-scale of the (mV << 15) / 1800 raw domain (1800 mV -> 0x8000).  Used to
 * test how close a raw averaged sample sits to either electrical rail for the
 * cross-axis deadzone below.  joy_rail_margin is how many raw units short of a
 * rail still counts as "railed"; both are runtime-tunable via sysfs so the
 * right-stick cross-axis compensation can be dialled in without a reflash.
 */
#define	JOY_ADC_FULLSCALE	0x8000
/*
 * "railed" band width (raw units): an axis counts as railed when its raw sample
 * is within joy_rail_margin of either rail (0 or 0x8000).  Default 8000 (~440 mV)
 * so a right-stick axis pushed past ~1360 mV / below ~440 mV (this panel tops out
 * near 1620 mV, not the 1800 mV electrical rail) engages the cross-axis median.
 */
static int joy_rail_margin = 8000;
/*
 * Spike-rejecting median filter for the stick axes.  On this hardware, when one
 * axis of a stick is driven to its mechanical extreme and the stick is MOVED,
 * the OTHER axis' potentiometer throws fast erratic outliers (+/-100..220 mV
 * bursts, measured; steady when held still), which the stock 4-sample mean only
 * smears and the x3 gain then amplifies into a large visible bounce (the "jumping
 * between 3 and 1 o'clock" on the right stick).  A median of the last
 * joy_median_win raw samples rejects those outliers where a mean cannot.  Both
 * knobs are runtime-tunable via sysfs so the window can be dialled in live.
 *   joy_median_win  : median window (odd, 3..JOY_HIST_MAX); < 3 => legacy mean-4.
 *   joy_median_railed: 1 => only median an axis while its PARTNER axis is railed
 *                      (keeps normal centred-region movement at the crisp mean-4);
 *                      0 => median always.
 * Defaults (device-tuned on the RG DS at the 500 Hz poll below): a 21-sample
 * window spans ~42 ms at 500 Hz, wide enough to outvote the ~19 ms spike bursts
 * with ~20 ms of group delay, and gated to the extremes so normal aim is crisp.
 */
#define	JOY_HIST_MAX		31
static int joy_median_win = 21;
static int joy_median_railed = 1;
/*
 * ABS fuzz floor for the analog stick axes, in gained report units.
 *
 * The stock kernel's runtime "mode2" slew-limiter (joypad_adc_check, only
 * armed via the touch/mouse sysfs flag) smooths the reported value against the
 * previous sample with the exact bands:
 *   |delta| <  320  -> hold previous
 *   320 <= |delta| <  640  -> (prev*3 + new) / 4
 *   640 <= |delta| < 1280  -> (prev + new) / 2
 *   |delta| >= 1280 -> accept new
 * That is bit-for-bit the Linux input core's input_defuzz_abs_event() run with
 * fuzz = 640 (its bands are fuzz/2, fuzz, fuzz*2 with the identical 3:1 and 1:1
 * weightings).  Feeding the input core fuzz = 640 therefore applies stock's own
 * tuned slew band via the identical kernel code.
 *
 * IMPORTANT: stock itself gates that slew behind a mouse/touch-mode sysfs flag
 * (off by default), so on a plain gamepad boot stock runs the RAW value and its
 * DT button-adc-fuzz = 1 is a no-op at this gain (fuzz/2 rounds to 0) -- which
 * is exactly the residual mid-throw jitter being reported.  Enabling the slew
 * unconditionally is a deliberate GammaOS choice to kill that jitter; it is a
 * jitter-vs-fine-aim trade (per-poll deltas below fuzz/2 = 320 are held), and
 * is tunable: lower JOY_ABS_FUZZ, or set a larger DT/boot.ini button-adc-fuzz
 * (which wins).  It smooths purely in the time domain toward the previous
 * sample, so it never pulls an axis toward a cardinal/diagonal (no snapping),
 * and real movement / endpoints produce deltas far above 1280 so travel and the
 * 0.98 full-scale snap are untouched.
 */
#define	JOY_ABS_FUZZ		640

struct bt_adc {
	/* report value (mV) */
	int value;
	/* report type */
	int report_type;
	/* input device init value (mV) */
	int max, min;
	/* calibrated adc value */
	int cal;
	/*  adc scale value */
	int scale;
	/* invert report */
	bool invert;
	/* amux channel */
	int amux_ch;
	/* adc data tuning value([percent), p = positive, n = negative */
	int tuning_p, tuning_n;
	/* raw-sample ring: mean-4 (legacy) or median (spike reject), see stage 1-4 */
	int hist[JOY_HIST_MAX];
	int hidx;
	/*
	 * Per-direction endpoint scale, ported from the stock RG DS jokstick_cal
	 * engine (jokstick_cal_init seeds all *_scale = 1000).  Positive
	 * deflection is scaled by cal_scale_pos, negative by cal_scale_neg, both
	 * as /1000 fixed point, applied before the global x3 gain.  At the
	 * identity default (1000/1000) this is a no-op and the reported value is
	 * byte-identical to the plain gain path.  It lets an axis whose electrical
	 * rest is far off centre (the RG DS right-stick horizontal rests at ~88%
	 * of scale) be normalised so both travel directions reach full scale
	 * instead of the short side over-gaining into the endpoint.
	 */
	int cal_scale_pos, cal_scale_neg;
	/*
	 * Cross-axis rest deadzone: extra deadzone (report units, pre-gain) added
	 * to THIS axis only while its partner stick axis is near an electrical
	 * rail.  On this panel driving one right-stick axis to a hardware extreme
	 * pulls the other axis a few tens of mV off centre, which the x3 gain and
	 * the 0.98 snap turn into a small wrong-signed report; widening the victim
	 * axis' deadzone only while its partner rails swallows that coupling
	 * without touching normal use or genuine diagonals.  Default 0 = inert.
	 */
	int cross_dz;
};

struct analog_mux {
	/* IIO ADC Channel : amux connect channel */
	struct iio_channel *iio_ch;
	/* analog mux select(a,b) gpio */
	int sel_a_gpio, sel_b_gpio;
	/* analog mux enable gpio */
	int en_gpio;
};

struct bt_gpio {
	/* GPIO Request label */
	const char *label;
	/* GPIO Number */
	int num;
	/* report type */
	int report_type;
	/* report linux code */
	int linux_code;
	/* DT linux,abs-value: for EV_ABS (D-pad HAT) buttons the pressed value
	 * to report (2 => -1, 1 => +1); unused for EV_KEY buttons */
	int abs_value;
	/* prev button value */
	bool old_value;
	/* button press level */
	bool active_level;
};

struct joypad {
	struct device *dev;
	int poll_interval;

	/* polled input device (input-polldev was removed in v5.6) */
	struct input_dev *input;

	/* report enable/disable */
	bool enable;

	/* analog mux & joystick control */
	struct analog_mux *amux;
	/* analog mux max count */
	int amux_count;
	/* analog button */
	struct bt_adc *adcs;

	/* report interval (ms) */
	int bt_gpio_count;
	struct bt_gpio *gpios;

	/* button auto repeat */
	int auto_repeat;

	/* report threshold (mV) */
	int bt_adc_fuzz, bt_adc_flat;
	/* adc read value scale */
	int bt_adc_scale;
	/* joystick deadzone control */
	int bt_adc_deadzone;

	struct mutex lock;

	/* amux debug channel */
	int debug_ch;
};

/*----------------------------------------------------------------------------*/
//
// set to the value in the boot.ini file. (if exist)
//
/*----------------------------------------------------------------------------*/
static unsigned int g_button_adc_fuzz = 0;
static unsigned int g_button_adc_flat = 0;
static unsigned int g_button_adc_scale = 0;
static unsigned int g_button_adc_deadzone = 0;

static int button_adc_fuzz(char *str)
{
	if (!str)
		return -EINVAL;
	g_button_adc_fuzz = simple_strtoul(str, NULL, 10);
	return 0;
}
__setup("button-adc-fuzz=", button_adc_fuzz);

static int button_adc_flat(char *str)
{
	if (!str)
		return -EINVAL;
	g_button_adc_flat = simple_strtoul(str, NULL, 10);
	return 0;
}
__setup("button-adc-flat=", button_adc_flat);

static int button_adc_scale(char *str)
{
	if (!str)
		return -EINVAL;
	g_button_adc_scale = simple_strtoul(str, NULL, 10);
	return 0;
}
__setup("button-adc-scale=", button_adc_scale);

static int button_adc_deadzone(char *str)
{
	if (!str)
		return -EINVAL;
	g_button_adc_deadzone = simple_strtoul(str, NULL, 10);
	return 0;
}
__setup("button-adc-deadzone=", button_adc_deadzone);

/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
static int joypad_amux_select(struct analog_mux *amux, int channel)
{
	/* select mux channel */
	gpio_set_value(amux->en_gpio, 0);

	switch(channel) {
		case 0:	/* EVENT (ABS_RY) */
			gpio_set_value(amux->sel_a_gpio, 0);
			gpio_set_value(amux->sel_b_gpio, 0);
			break;
		case 1:	/* EVENT (ABS_RX) */
			gpio_set_value(amux->sel_a_gpio, 0);
			gpio_set_value(amux->sel_b_gpio, 1);
			break;
		case 2:	/* EVENT (ABS_Y) */
			gpio_set_value(amux->sel_a_gpio, 1);
			gpio_set_value(amux->sel_b_gpio, 0);
			break;
		case 3:	/* EVENT (ABS_X) */
			gpio_set_value(amux->sel_a_gpio, 1);
			gpio_set_value(amux->sel_b_gpio, 1);
			break;
		default:
			/* amux disanle */
			gpio_set_value(amux->en_gpio, 1);
			return -1;
	}
	/* mux swtiching speed : 35ns(on) / 9ns(off) */
	usleep_range(10, 20);
	return 0;
}

/*----------------------------------------------------------------------------*/
static int joypad_adc_read(struct analog_mux *amux, struct bt_adc *adc)
{
	int value = 0;

	if (joypad_amux_select(amux, adc->amux_ch))
		return 0;

	/*
	 * Stock reads the PROCESSED (millivolt) IIO value, not raw counts, and
	 * maps 0..1800 mV onto 0..0x8000 via (mV << 15) / 1800 (divisor exact).
	 * Range enforcement, deadzone and invert are all done in
	 * joypad_adc_check(), matching stock; this returns a pure linear value.
	 */
	iio_read_channel_processed(amux->iio_ch, &value);

	return (int)(((s64)value << 15) / ADC_MAX_VOLTAGE);
}

/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
/*
 * ATTRIBUTES:
 *
 * /sys/devices/platform/retrogame_joypad/poll_interval [rw]
 */
/*----------------------------------------------------------------------------*/
static ssize_t joypad_store_poll_interval(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t count)
{
	struct platform_device *pdev  = to_platform_device(dev);
	struct joypad *joypad = platform_get_drvdata(pdev);

	mutex_lock(&joypad->lock);
	joypad->poll_interval = simple_strtoul(buf, NULL, 10);
	mutex_unlock(&joypad->lock);

	return count;
}

/*----------------------------------------------------------------------------*/
static ssize_t joypad_show_poll_interval(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct platform_device *pdev  = to_platform_device(dev);
	struct joypad *joypad = platform_get_drvdata(pdev);

	return sprintf(buf, "%d\n", joypad->poll_interval);
}

/*----------------------------------------------------------------------------*/
static DEVICE_ATTR(poll_interval, S_IWUSR | S_IRUGO,
		   joypad_show_poll_interval,
		   joypad_store_poll_interval);

/*----------------------------------------------------------------------------*/
/*
 * ATTRIBUTES:
 *
 * /sys/devices/platform/retrogame_joypad/adc_fuzz [r]
 */
/*----------------------------------------------------------------------------*/
static ssize_t joypad_show_adc_fuzz(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct platform_device *pdev  = to_platform_device(dev);
	struct joypad *joypad = platform_get_drvdata(pdev);

	return sprintf(buf, "%d\n", joypad->bt_adc_fuzz);
}

/*----------------------------------------------------------------------------*/
static DEVICE_ATTR(adc_fuzz, S_IWUSR | S_IRUGO,
		   joypad_show_adc_fuzz,
		   NULL);

/*----------------------------------------------------------------------------*/
/*
 * ATTRIBUTES:
 *
 * /sys/devices/platform/retrogame_joypad/adc_flat [r]
 */
/*----------------------------------------------------------------------------*/
static ssize_t joypad_show_adc_flat(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct platform_device *pdev  = to_platform_device(dev);
	struct joypad *joypad = platform_get_drvdata(pdev);

	return sprintf(buf, "%d\n", joypad->bt_adc_flat);
}

/*----------------------------------------------------------------------------*/
static DEVICE_ATTR(adc_flat, S_IWUSR | S_IRUGO,
		   joypad_show_adc_flat,
		   NULL);

/*----------------------------------------------------------------------------*/
/*
 * ATTRIBUTES:
 *
 * /sys/devices/platform/retrogame_joypad/enable [rw]
 */
/*----------------------------------------------------------------------------*/
static ssize_t joypad_store_enable(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t count)
{
	struct platform_device *pdev  = to_platform_device(dev);
	struct joypad *joypad = platform_get_drvdata(pdev);

	mutex_lock(&joypad->lock);
	joypad->enable = simple_strtoul(buf, NULL, 10);
	mutex_unlock(&joypad->lock);

	return count;
}

/*----------------------------------------------------------------------------*/
static ssize_t joypad_show_enable(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct platform_device *pdev  = to_platform_device(dev);
	struct joypad *joypad = platform_get_drvdata(pdev);

	return sprintf(buf, "%d\n", joypad->enable);
}

/*----------------------------------------------------------------------------*/
static DEVICE_ATTR(enable, S_IWUSR | S_IRUGO,
		   joypad_show_enable,
		   joypad_store_enable);

/*----------------------------------------------------------------------------*/
/*
 * ATTRIBUTES:
 *
 * /sys/devices/platform/retrogame_joypad/adc_cal [rw]
 */
/*----------------------------------------------------------------------------*/
static ssize_t joypad_store_adc_cal(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t count)
{
	struct platform_device *pdev  = to_platform_device(dev);
	struct joypad *joypad = platform_get_drvdata(pdev);
	bool calibration;

	calibration = simple_strtoul(buf, NULL, 10);

	if (calibration) {
		int nbtn;

		mutex_lock(&joypad->lock);
		for (nbtn = 0; nbtn < joypad->amux_count; nbtn++) {
			struct bt_adc *adc = &joypad->adcs[nbtn];

			adc->value = joypad_adc_read(joypad->amux, adc);
			if (!adc->value) {
				dev_err(joypad->dev, "%s : saradc channels[%d]!\n",
					__func__, nbtn);
				continue;
			}
			adc->cal = adc->value;
		}
		mutex_unlock(&joypad->lock);
	}
	return count;
}

/*----------------------------------------------------------------------------*/
static ssize_t joypad_show_adc_cal(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct platform_device *pdev  = to_platform_device(dev);
	struct joypad *joypad = platform_get_drvdata(pdev);
	int nbtn;
	ssize_t pos;

	for (nbtn = 0, pos = 0; nbtn < joypad->amux_count; nbtn++) {
		struct bt_adc *adc = &joypad->adcs[nbtn];
		pos += sprintf(&buf[pos], "adc[%d]->cal = %d\n",
				nbtn, adc->cal);
	}
	pos += sprintf(&buf[pos], "adc scale = %d\n", joypad->bt_adc_scale);
	return pos;
}

/*----------------------------------------------------------------------------*/
static DEVICE_ATTR(adc_cal, S_IWUSR | S_IRUGO,
		   joypad_show_adc_cal,
		   joypad_store_adc_cal);

/*----------------------------------------------------------------------------*/
/*
 * ATTRIBUTES:
 *
 * /sys/devices/platform/retrogame_joypad/amux_debug [rw]
 *
 * echo [debug channel] > amux_debug
 * cat amux_debug : debug channel mux set & adc read
 */
/*----------------------------------------------------------------------------*/
static ssize_t joypad_store_amux_debug(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t count)
{
	struct platform_device *pdev  = to_platform_device(dev);
	struct joypad *joypad = platform_get_drvdata(pdev);

	joypad->debug_ch = simple_strtoul(buf, NULL, 10);

	/* if error than default setting(debug_ch = 0) */
	if (joypad->debug_ch > joypad->amux_count)
		joypad->debug_ch = 0;

	return count;
}

/*----------------------------------------------------------------------------*/
static ssize_t joypad_show_amux_debug(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct platform_device *pdev  = to_platform_device(dev);
	struct joypad *joypad = platform_get_drvdata(pdev);
	struct analog_mux *amux = joypad->amux;
	ssize_t pos;
	int value;

	mutex_lock(&joypad->lock);

	/* disable poll driver */
	if (joypad->enable)
		joypad->enable = false;

	if (joypad_amux_select(amux, joypad->debug_ch))
		goto err_out;

	if (iio_read_channel_processed(amux->iio_ch, &value))
		goto err_out;

	pos = sprintf(buf, "amux ch[%d], adc scale = %d, adc value = %d\n",
			joypad->debug_ch, joypad->bt_adc_scale,
			value * joypad->bt_adc_scale);
	goto out;

err_out:
	pos = sprintf(buf, "error : amux setup & adc read!\n");
out:
	mutex_unlock(&joypad->lock);
	return pos;
}

/*----------------------------------------------------------------------------*/
static DEVICE_ATTR(amux_debug, S_IWUSR | S_IRUGO,
		   joypad_show_amux_debug,
		   joypad_store_amux_debug);

/*----------------------------------------------------------------------------*/
/*
 * Right-stick calibration knobs (live-tunable, so the per-direction endpoint
 * scale and the cross-axis deadzone can be dialled in against a real sweep
 * without a reflash; once confirmed the values are baked into the identity
 * seeds in joypad_adc_setup).  Everything defaults to the identity (scale 1000,
 * cross_dz 0), so these are no-ops until written.
 *
 *   .../singleadc-joypad/rx_scale_pos  rx_scale_neg   (ABS_RX per-direction /1000)
 *   .../singleadc-joypad/ry_scale_pos  ry_scale_neg   (ABS_RY per-direction /1000)
 *   .../singleadc-joypad/rx_cross_dz   ry_cross_dz    (extra dz while partner rails)
 *   .../singleadc-joypad/rail_margin                  (raw units short of a rail)
 */
static struct bt_adc *joypad_adc_by_type(struct joypad *joypad, int type)
{
	int n;

	for (n = 0; n < joypad->amux_count && n < 4; n++)
		if (joypad->adcs[n].report_type == type)
			return &joypad->adcs[n];
	return NULL;
}

#define JOY_CAL_ATTR(_name, _type, _field)				\
static ssize_t joypad_show_##_name(struct device *dev,			\
		struct device_attribute *attr, char *buf)		\
{									\
	struct platform_device *pdev = to_platform_device(dev);		\
	struct joypad *joypad = platform_get_drvdata(pdev);		\
	struct bt_adc *adc = joypad_adc_by_type(joypad, _type);		\
	return sprintf(buf, "%d\n", adc ? adc->_field : 0);		\
}									\
static ssize_t joypad_store_##_name(struct device *dev,			\
		struct device_attribute *attr,				\
		const char *buf, size_t count)				\
{									\
	struct platform_device *pdev = to_platform_device(dev);		\
	struct joypad *joypad = platform_get_drvdata(pdev);		\
	struct bt_adc *adc = joypad_adc_by_type(joypad, _type);		\
	int v = (int)simple_strtoul(buf, NULL, 10);				\
	if (adc) {							\
		mutex_lock(&joypad->lock);				\
		adc->_field = v;					\
		mutex_unlock(&joypad->lock);				\
	}								\
	return count;							\
}									\
static DEVICE_ATTR(_name, S_IWUSR | S_IRUGO,				\
		   joypad_show_##_name, joypad_store_##_name)

JOY_CAL_ATTR(rx_scale_pos, ABS_RX, cal_scale_pos);
JOY_CAL_ATTR(rx_scale_neg, ABS_RX, cal_scale_neg);
JOY_CAL_ATTR(ry_scale_pos, ABS_RY, cal_scale_pos);
JOY_CAL_ATTR(ry_scale_neg, ABS_RY, cal_scale_neg);
JOY_CAL_ATTR(rx_cross_dz,  ABS_RX, cross_dz);
JOY_CAL_ATTR(ry_cross_dz,  ABS_RY, cross_dz);

static ssize_t joypad_show_rail_margin(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", joy_rail_margin);
}
static ssize_t joypad_store_rail_margin(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	joy_rail_margin = (int)simple_strtoul(buf, NULL, 10);
	return count;
}
static DEVICE_ATTR(rail_margin, S_IWUSR | S_IRUGO,
		   joypad_show_rail_margin, joypad_store_rail_margin);

static ssize_t joypad_show_median_win(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", joy_median_win);
}
static ssize_t joypad_store_median_win(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int v = (int)simple_strtoul(buf, NULL, 10);

	if (v > JOY_HIST_MAX)
		v = JOY_HIST_MAX;
	if (v >= 3 && !(v & 1))		/* force odd so the median has a centre */
		v -= 1;
	joy_median_win = v;
	return count;
}
static DEVICE_ATTR(median_win, S_IWUSR | S_IRUGO,
		   joypad_show_median_win, joypad_store_median_win);

static ssize_t joypad_show_median_railed(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", joy_median_railed);
}
static ssize_t joypad_store_median_railed(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	joy_median_railed = simple_strtoul(buf, NULL, 10) ? 1 : 0;
	return count;
}
static DEVICE_ATTR(median_railed, S_IWUSR | S_IRUGO,
		   joypad_show_median_railed, joypad_store_median_railed);

/*----------------------------------------------------------------------------*/
#ifdef __MURMUR__
/*----------------------------------------------------------------------------*/
/*
 * ATTRIBUTES:
 *
 * /sys/bus/platform/devices/singleadc-joypad/vol [rw]
 *
 * echo [debug channel] > vol
 * cat vol : debug channel mux set & adc read
 */
/*----------------------------------------------------------------------------*/

extern int rk817_hp_inserted;

int rk817_pa_power_on=1;
EXPORT_SYMBOL(rk817_pa_power_on);

int vol_temp=100;

static ssize_t joypad_store_vol(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t count)
{	

	vol_temp = simple_strtoul(buf, NULL, 10);

	if(vol_temp>=15){
		rk817_pa_power_on=1;
		if(rk817_hp_inserted==0){
			gpio_direction_output(146,1);	
		}
		
	}else{
		rk817_pa_power_on=0;

		if(rk817_hp_inserted==0){
			gpio_direction_output(146,0);	
		}
	}

	return count;
}

/*----------------------------------------------------------------------------*/
static ssize_t joypad_show_vol(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	ssize_t pos;
	pos = sprintf(buf, "%d\n",vol_temp);
	return pos;
}

/*----------------------------------------------------------------------------*/
static DEVICE_ATTR(vol, S_IWUSR | S_IRUGO,joypad_show_vol, joypad_store_vol);

/*----------------------------------------------------------------------------*/
#endif
#ifdef __MURMUR__
/*----------------------------------------------------------------------------*/
static struct attribute *joypad_attrs[] = {
	&dev_attr_poll_interval.attr,
	&dev_attr_adc_fuzz.attr,
	&dev_attr_adc_flat.attr,
	&dev_attr_enable.attr,
	&dev_attr_adc_cal.attr,
	&dev_attr_amux_debug.attr,
	&dev_attr_vol.attr,
	&dev_attr_rx_scale_pos.attr,
	&dev_attr_rx_scale_neg.attr,
	&dev_attr_ry_scale_pos.attr,
	&dev_attr_ry_scale_neg.attr,
	&dev_attr_rx_cross_dz.attr,
	&dev_attr_ry_cross_dz.attr,
	&dev_attr_rail_margin.attr,
	&dev_attr_median_win.attr,
	&dev_attr_median_railed.attr,
	NULL,
};
#else
/*----------------------------------------------------------------------------*/
static struct attribute *joypad_attrs[] = {
	&dev_attr_poll_interval.attr,
	&dev_attr_adc_fuzz.attr,
	&dev_attr_adc_flat.attr,
	&dev_attr_enable.attr,
	&dev_attr_adc_cal.attr,
	&dev_attr_amux_debug.attr,
	&dev_attr_rx_scale_pos.attr,
	&dev_attr_rx_scale_neg.attr,
	&dev_attr_ry_scale_pos.attr,
	&dev_attr_ry_scale_neg.attr,
	&dev_attr_rx_cross_dz.attr,
	&dev_attr_ry_cross_dz.attr,
	&dev_attr_rail_margin.attr,
	&dev_attr_median_win.attr,
	&dev_attr_median_railed.attr,
	NULL,
};
#endif

static struct attribute_group joypad_attr_group = {
	.attrs = joypad_attrs,
};

/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
static void joypad_gpio_check(struct joypad *joypad)
{
	int nbtn, value;

	for (nbtn = 0; nbtn < joypad->bt_gpio_count; nbtn++) {
		struct bt_gpio *gpio = &joypad->gpios[nbtn];

		if (gpio_get_value_cansleep(gpio->num) < 0) {
			dev_err(joypad->dev, "failed to get gpio state\n");
			continue;
		}
		value = gpio_get_value(gpio->num);
		if (value != gpio->old_value) {
			int report;

			if (gpio->report_type == EV_ABS) {
				/* D-pad reported as an ABS_HAT axis: two opposing
				 * buttons share one code, so the DT linux,abs-value
				 * encodes the direction (2 => negative, 1 =>
				 * positive). Emit the signed value on press, 0 on
				 * release. */
				if (value == gpio->active_level)
					report = (gpio->abs_value == 2) ?
							-1 : gpio->abs_value;
				else
					report = 0;
			} else {
				report = (value == gpio->active_level) ? 1 : 0;
			}
			input_event(joypad->input,
				gpio->report_type,
				gpio->linux_code,
				report);
			gpio->old_value = value;
		}
	}
	input_sync(joypad->input);
}

/*----------------------------------------------------------------------------*/
static void joypad_adc_check(struct joypad *joypad)
{
	int nbtn;
	int centred[4] = { 0 };
	int absv[4] = { 0 };

	/*
	 * Stage 1-4: one fresh sample per stick channel into a raw ring, then
	 * either the stock 4-sample mean or a spike-rejecting median (see the
	 * joy_median_* knobs above), then centre-subtract against the rest
	 * calibration.  |deviation| is cached for the per-axis deadzone below.
	 * Channels >= 4 are unused trigger slots that stock forces to rest.
	 *
	 * Two passes so the median can be gated on the PARTNER axis being railed:
	 * pass A reads every channel (so each partner's fresh raw is known), pass
	 * B filters.  nstick counts the stick channels present.
	 */
	int rawnow[4] = { 0 };
	int nstick = 0;

	for (nbtn = 0; nbtn < joypad->amux_count && nbtn < 4; nbtn++) {
		struct bt_adc *adc = &joypad->adcs[nbtn];

		rawnow[nbtn] = joypad_adc_read(joypad->amux, adc);
		adc->hist[adc->hidx] = rawnow[nbtn];
		adc->hidx = (adc->hidx + 1) % JOY_HIST_MAX;
		nstick = nbtn + 1;
	}

	for (nbtn = 0; nbtn < nstick; nbtn++) {
		struct bt_adc *adc = &joypad->adcs[nbtn];
		int win = joy_median_win;
		bool use_median = (win >= 3);

		if (use_median && joy_median_railed && (nbtn ^ 1) < nstick) {
			int praw = rawnow[nbtn ^ 1];

			/* partner not near a rail -> keep the crisp mean */
			if (praw > joy_rail_margin &&
			    praw < (JOY_ADC_FULLSCALE - joy_rail_margin))
				use_median = false;
		}

		if (use_median) {
			int tmp[JOY_HIST_MAX];
			int i, j, key;

			if (win > JOY_HIST_MAX)
				win = JOY_HIST_MAX;
			for (i = 0; i < win; i++)
				tmp[i] = adc->hist[(adc->hidx - 1 - i +
						    JOY_HIST_MAX) % JOY_HIST_MAX];
			for (i = 1; i < win; i++) {	/* insertion sort */
				key = tmp[i];
				for (j = i - 1; j >= 0 && tmp[j] > key; j--)
					tmp[j + 1] = tmp[j];
				tmp[j + 1] = key;
			}
			adc->value = tmp[win / 2];
		} else {
			int sum = 0, i;

			for (i = 0; i < JOY_AVG_DEPTH; i++)
				sum += adc->hist[(adc->hidx - 1 - i +
						  JOY_HIST_MAX) % JOY_HIST_MAX];
			adc->value = sum / JOY_AVG_DEPTH;
		}

		centred[nbtn] = adc->value - adc->cal;
		absv[nbtn] = abs(centred[nbtn]);
	}

	/*
	 * Stage 5: PER-AXIS rest deadzone (+ cross-axis widening) -> per-direction
	 * endpoint scale -> global x3 gain -> 0.98 endpoint snap.
	 *
	 * Each axis is zeroed by its OWN deviation, independent of the other axis
	 * on the same stick.  Stock uses a radial test ("stick live if EITHER axis
	 * leaves the deadzone"), but on this hardware that leaks: at an X extreme
	 * the right stick's X rails, so a radial test keeps Y "live" even when Y is
	 * physically centred -- and Y's small rest offset (its centre shifts and
	 * its range compresses at the rail) plus rest-noise then gets the x3 gain
	 * and shows up as the reported Y drifting/twitching (and feeling inverted)
	 * at the left/right extremes.  A per-axis deadzone zeroes that idle-axis
	 * wobble; a genuine diagonal (both axes past the deadzone) is still
	 * reported on both axes, so real diagonal travel is unaffected.
	 *
	 * On top of the flat per-axis deadzone we add two ported-from-stock levers,
	 * both inert at their identity defaults so this is byte-identical to the
	 * plain path until the right stick is tuned:
	 *   - cross_dz: while an axis' PARTNER (nbtn ^ 1) sits near an electrical
	 *     rail, this axis' deadzone is widened by cross_dz.  The RG DS right
	 *     stick's cross-axis coupling only appears at the partner's extreme, so
	 *     widening the victim's deadzone only then swallows the coupling
	 *     without costing fine aim anywhere else.
	 *   - cal_scale_pos/neg: stock's per-direction endpoint scale.  The right
	 *     stick's horizontal rests at ~88% of scale, so its two travel
	 *     directions are wildly asymmetric; an independent scale per direction
	 *     lets both directions reach full scale instead of the short side
	 *     over-gaining into the endpoint (which is what flips the sign).
	 *
	 * The x3.0 gain still overdrives and the 0.98-of-full-scale snap still
	 * catches the hard endpoint, guaranteeing full travel; each axis snaps
	 * against its own min/max.
	 */
	for (nbtn = 0; nbtn < joypad->amux_count && nbtn < 4; nbtn++) {
		struct bt_adc *adc = &joypad->adcs[nbtn];
		int val = centred[nbtn];
		int dz = JOY_CAL_DEADZONE;
		int dir_scale;

		if (adc->cross_dz && (nbtn ^ 1) < 4) {
			int praw = centred[nbtn ^ 1] +
				   joypad->adcs[nbtn ^ 1].cal;

			if (praw <= joy_rail_margin ||
			    praw >= (JOY_ADC_FULLSCALE - joy_rail_margin))
				dz += adc->cross_dz;
		}

		if (absv[nbtn] <= dz)
			val = 0;

		dir_scale = (val >= 0) ? adc->cal_scale_pos : adc->cal_scale_neg;
		val = (int)(((s64)val * dir_scale) / 1000);

		val = (int)(((s64)JOY_CAL_SCALE * val) / 1000);

		if (val > (adc->max * 98) / 100)
			val = adc->max;
		else if (val < (adc->min * 98) / 100)
			val = adc->min;

		adc->value = val;
		input_report_abs(joypad->input, adc->report_type,
				 adc->invert ? -val : val);
	}

	/* Unused trigger slots (>= 4): report rest, matching stock. */
	for (nbtn = 4; nbtn < joypad->amux_count; nbtn++) {
		struct bt_adc *adc = &joypad->adcs[nbtn];

		adc->value = 0;
		input_report_abs(joypad->input, adc->report_type, 0);
	}

	input_sync(joypad->input);
}

/*----------------------------------------------------------------------------*/
static void joypad_poll(struct input_dev *input)
{
	struct joypad *joypad = input_get_drvdata(input);

	if (joypad->enable) {
		joypad_adc_check(joypad);
		joypad_gpio_check(joypad);
	}
	if (input_get_poll_interval(input) != joypad->poll_interval) {
		mutex_lock(&joypad->lock);
		input_set_poll_interval(input, joypad->poll_interval);
		mutex_unlock(&joypad->lock);
	}
}

/*----------------------------------------------------------------------------*/
static int joypad_open(struct input_dev *input)
{
	struct joypad *joypad = input_get_drvdata(input);
	int nbtn;

	for (nbtn = 0; nbtn < joypad->bt_gpio_count; nbtn++) {
		struct bt_gpio *gpio = &joypad->gpios[nbtn];
		gpio->old_value = gpio->active_level ? 0 : 1;
	}
	for (nbtn = 0; nbtn < joypad->amux_count; nbtn++) {
		struct bt_adc *adc = &joypad->adcs[nbtn];

		adc->value = joypad_adc_read(joypad->amux, adc);
		if (!adc->value) {
			dev_err(joypad->dev, "%s : saradc channels[%d]!\n",
				__func__, nbtn);
			continue;
		}
		adc->cal = adc->value;
		/* prime the whole raw ring with the rest read so the median /
		 * mean is stable from the first poll (no wrap-to-zero glitch). */
		for (int h = 0; h < JOY_HIST_MAX; h++)
			adc->hist[h] = adc->value;
		adc->hidx = 0;
		dev_info(joypad->dev, "%s : adc[%d] adc->cal = %d\n",
			__func__, nbtn, adc->cal);
	}
	/* buttons status sync */
	joypad_adc_check(joypad);
	joypad_gpio_check(joypad);

	/* button report enable */
	mutex_lock(&joypad->lock);
	joypad->enable = true;
	mutex_unlock(&joypad->lock);

	dev_info(joypad->dev, "%s : opened\n", __func__);
	return 0;
}

/*----------------------------------------------------------------------------*/
static void joypad_close(struct input_dev *input)
{
	struct joypad *joypad = input_get_drvdata(input);

	/* button report disable */
	mutex_lock(&joypad->lock);
	joypad->enable = false;
	mutex_unlock(&joypad->lock);

	dev_info(joypad->dev, "%s : closed\n", __func__);
}

/*----------------------------------------------------------------------------*/
static int joypad_amux_setup(struct device *dev, struct joypad *joypad)
{
	struct analog_mux *amux;
	enum iio_chan_type type;
	enum of_gpio_flags flags;
	int ret;

	/* analog mux control struct init */
	joypad->amux = devm_kzalloc(dev, sizeof(struct analog_mux),
					GFP_KERNEL);
	if (!joypad->amux) {
		dev_err(dev, "%s amux devm_kzmalloc error!", __func__);
		return -ENOMEM;
	}
	amux = joypad->amux;
	amux->iio_ch = devm_iio_channel_get(dev, "amux_adc");
	if (IS_ERR(amux->iio_ch)) {
		dev_err(dev, "iio channel get error\n");
		return -EINVAL;
	}
	if (!amux->iio_ch->indio_dev)
		return -ENXIO;

	if (iio_get_channel_type(amux->iio_ch, &type))
		return -EINVAL;

	if (type != IIO_VOLTAGE) {
		dev_err(dev, "Incompatible channel type %d\n", type);
		return -EINVAL;
	}

	amux->sel_a_gpio = of_get_named_gpio_flags(dev->of_node,
				"amux-a-gpios", 0, &flags);
	if (gpio_is_valid(amux->sel_a_gpio)) {
		ret = devm_gpio_request(dev, amux->sel_a_gpio, "amux-sel-a");
		if (ret < 0) {
			dev_err(dev, "%s : failed to request amux-sel-a %d\n",
				__func__, amux->sel_a_gpio);
			goto err_out;
		}
		ret = gpio_direction_output(amux->sel_a_gpio, 0);
		if (ret < 0)
			goto err_out;
	}

	amux->sel_b_gpio = of_get_named_gpio_flags(dev->of_node,
				"amux-b-gpios", 0, &flags);
	if (gpio_is_valid(amux->sel_b_gpio)) {
		ret = devm_gpio_request(dev, amux->sel_b_gpio, "amux-sel-b");
		if (ret < 0) {
			dev_err(dev, "%s : failed to request amux-sel-b %d\n",
				__func__, amux->sel_b_gpio);
			goto err_out;
		}
		ret = gpio_direction_output(amux->sel_b_gpio, 0);
		if (ret < 0)
			goto err_out;
	}

	amux->en_gpio = of_get_named_gpio_flags(dev->of_node,
				"amux-en-gpios", 0, &flags);
	if (gpio_is_valid(amux->en_gpio)) {
		ret = devm_gpio_request(dev, amux->en_gpio, "amux-en");
		if (ret < 0) {
			dev_err(dev, "%s : failed to request amux-en %d\n",
				__func__, amux->en_gpio);
			goto err_out;
		}
		ret = gpio_direction_output(amux->en_gpio, 0);
		if (ret < 0)
			goto err_out;
	}
	return	0;
err_out:
	return ret;
}

/*----------------------------------------------------------------------------*/
static int joypad_adc_setup(struct device *dev, struct joypad *joypad)
{
	int nbtn;

	/* adc button struct init */
	joypad->adcs = devm_kzalloc(dev, joypad->amux_count *
				sizeof(struct bt_adc), GFP_KERNEL);
	if (!joypad->adcs) {
		dev_err(dev, "%s devm_kzmalloc error!", __func__);
		return -ENOMEM;
	}

	for (nbtn = 0; nbtn < joypad->amux_count; nbtn++) {
		struct bt_adc *adc = &joypad->adcs[nbtn];

		adc->scale = joypad->bt_adc_scale;
		
		/* Sticks (ch0..3) are bipolar; triggers (ch4/5 ABS_Z/RZ) are
		 * unipolar 0..range, matching the stock kernel. */
		adc->max = ADC_ABS_RANGE;
		adc->min = (nbtn < 4) ? -ADC_ABS_RANGE : 0;
		if (adc->scale) {
			adc->max *= adc->scale;
			adc->min *= adc->scale;
		}
		adc->amux_ch = nbtn;
		adc->invert = false;

		/*
		 * Identity calibration defaults, matching the stock jokstick_cal
		 * seeds (all *_scale = 1000, no cross-axis deadzone).  With these
		 * the processing is byte-identical to the plain gain path; the
		 * right-stick values are tuned later (via sysfs, then baked here)
		 * once the panel's per-direction spans and cross-axis coupling
		 * have been measured.
		 */
		adc->cal_scale_pos = 1000;
		adc->cal_scale_neg = 1000;
		adc->cross_dz = 0;

		switch (nbtn) {
			/*
			 * Base driver mapping, with the RIGHT stick's two channels
			 * swapped (ch0<->ch1): ch0=ABS_RX, ch1=ABS_RY. That is the
			 * only change needed to un-rotate the right stick. Left stick
			 * (ch2=ABS_Y, ch3=ABS_X) is unchanged from base.
			 */
			case 0:
				adc->report_type = ABS_RX;
				if (device_property_read_u32(dev,
					"abs_rx-p-tuning",
					&adc->tuning_p))
					adc->tuning_p = ADC_TUNING_DEFAULT;
				if (device_property_read_u32(dev,
					"abs_rx-n-tuning",
					&adc->tuning_n))
					adc->tuning_n = ADC_TUNING_DEFAULT;
				break;
			case 1:
				/*
				 * Right stick up/down (ABS_RY) reads inverted on the
				 * RG DS (pushing up reports down); flip it, matching
				 * how the left stick axes are inverted above.  X
				 * (ABS_RX, case 0) is correct and left untouched.
				 */
				adc->invert = true;
				adc->report_type = ABS_RY;
				if (device_property_read_u32(dev,
					"abs_ry-p-tuning",
					&adc->tuning_p))
					adc->tuning_p = ADC_TUNING_DEFAULT;
				if (device_property_read_u32(dev,
					"abs_ry-n-tuning",
					&adc->tuning_n))
					adc->tuning_n = ADC_TUNING_DEFAULT;
				break;
			case 2:
			#ifdef __LEFT_JOYSTICK_INVERT__
				adc->invert = true;
			#endif
				adc->report_type = ABS_Y;
				if (device_property_read_u32(dev,
					"abs_y-p-tuning",
					&adc->tuning_p))
					adc->tuning_p = ADC_TUNING_DEFAULT;
				if (device_property_read_u32(dev,
					"abs_y-n-tuning",
					&adc->tuning_n))
					adc->tuning_n = ADC_TUNING_DEFAULT;
				break;
			case 3:
			#ifdef __LEFT_JOYSTICK_INVERT__
				adc->invert = true;
			#endif
				adc->report_type = ABS_X;
				if (device_property_read_u32(dev,
					"abs_x-p-tuning",
					&adc->tuning_p))
					adc->tuning_p = ADC_TUNING_DEFAULT;
				if (device_property_read_u32(dev,
					"abs_x-n-tuning",
					&adc->tuning_n))
					adc->tuning_n = ADC_TUNING_DEFAULT;
				break;
			case 4:
				adc->report_type = ABS_Z;
				if (device_property_read_u32(dev,
					"abs_z-p-tuning",
					&adc->tuning_p))
					adc->tuning_p = ADC_TUNING_DEFAULT;
				if (device_property_read_u32(dev,
					"abs_z-n-tuning",
					&adc->tuning_n))
					adc->tuning_n = ADC_TUNING_DEFAULT;
				break;
			case 5:
				adc->report_type = ABS_RZ;
				if (device_property_read_u32(dev,
					"abs_rz-p-tuning",
					&adc->tuning_p))
					adc->tuning_p = ADC_TUNING_DEFAULT;
				if (device_property_read_u32(dev,
					"abs_rz-n-tuning",
					&adc->tuning_n))
					adc->tuning_n = ADC_TUNING_DEFAULT;
				break;
			default :
				dev_err(dev, "%s amux count(%d) error!",
					__func__, nbtn);
				return -EINVAL;
		}
	}
	return	0;
}

/*----------------------------------------------------------------------------*/
static int joypad_gpio_setup(struct device *dev, struct joypad *joypad)
{
	struct device_node *node, *pp;
	int nbtn;

	node = dev->of_node;
	if (!node)
		return -ENODEV;

	joypad->gpios = devm_kzalloc(dev, joypad->bt_gpio_count *
				sizeof(struct bt_gpio), GFP_KERNEL);

	if (!joypad->gpios) {
		dev_err(dev, "%s devm_kzmalloc error!", __func__);
		return -ENOMEM;
	}

	nbtn = 0;
	for_each_child_of_node(node, pp) {
		enum of_gpio_flags flags;
		struct bt_gpio *gpio = &joypad->gpios[nbtn++];
		int error;

		gpio->num = of_get_gpio_flags(pp, 0, &flags);
		if (gpio->num < 0) {
			error = gpio->num;
			dev_err(dev, "Failed to get gpio flags, error: %d\n",
				error);
			return error;
		}

		/* gpio active level(key press level) */
		gpio->active_level = (flags & OF_GPIO_ACTIVE_LOW) ? 0 : 1;

		gpio->label = of_get_property(pp, "label", NULL);

		if (gpio_is_valid(gpio->num)) {
			error = devm_gpio_request_one(dev, gpio->num,
						      GPIOF_IN, gpio->label);
			if (error < 0) {
				dev_err(dev,
					"Failed to request GPIO %d, error %d\n",
					gpio->num, error);
				return error;
			}
		}
		if (of_property_read_u32(pp, "linux,code", &gpio->linux_code)) {
			dev_err(dev, "Button without keycode: 0x%x\n",
				gpio->num);
			return -EINVAL;
		}
		if (of_property_read_u32(pp, "linux,input-type",
				&gpio->report_type))
			gpio->report_type = EV_KEY;
		if (of_property_read_u32(pp, "linux,abs-value",
				&gpio->abs_value))
			gpio->abs_value = 1;
	}
	if (nbtn == 0)
		return -EINVAL;

	return	0;
}

/*----------------------------------------------------------------------------*/
struct input_dev * joypad_input_g;


void rk_send_key_f_key_up(void)
{
	if (!joypad_input_g)
		return;

	input_report_key(joypad_input_g, BTN_MODE, 1);
	input_sync(joypad_input_g);
}
EXPORT_SYMBOL(rk_send_key_f_key_up);


void rk_send_key_f_key_down(void)
{
	if (!joypad_input_g)
		return;

	input_report_key(joypad_input_g, BTN_MODE, 0);
	input_sync(joypad_input_g);
}
EXPORT_SYMBOL(rk_send_key_f_key_down);


static int joypad_input_setup(struct device *dev, struct joypad *joypad)
{
	struct input_dev *input;
	int nbtn, error;
	u32 joypad_revision = 0;
	u32 joypad_product = 0;

	input = devm_input_allocate_device(dev);
	if (!input) {
		dev_err(dev, "no memory for input device\n");
		return -ENOMEM;
	}

	joypad->input = input;
	input_set_drvdata(input, joypad);
	input->open  = joypad_open;
	input->close = joypad_close;
	joypad_input_g = input;

	device_property_read_string(dev, "joypad-name", &input->name);
	input->phys = DRV_NAME"/input0";

	device_property_read_u32(dev, "joypad-revision", &joypad_revision);
	device_property_read_u32(dev, "joypad-product", &joypad_product);
	input->id.bustype = BUS_HOST;
	input->id.vendor  = 0x484B;
	input->id.product = (u16)joypad_product;
	input->id.version = (u16)joypad_revision;

	/* IIO ADC key setup (0 mv ~ 1800 mv) * adc->scale */
	__set_bit(EV_ABS, input->evbit);
	for(nbtn = 0; nbtn < joypad->amux_count; nbtn++) {
		struct bt_adc *adc = &joypad->adcs[nbtn];
		int fuzz = joypad->bt_adc_fuzz;

		/*
		 * The stick axes (ch0..3) are reported in the gained 0..0x4000
		 * domain, where the DT's button-adc-fuzz (1) is a no-op and the
		 * amplified SARADC noise shows up as on-screen jitter when the
		 * stick is held part-way.  Give the input core the stock mode2
		 * slew band (JOY_ABS_FUZZ) so its input_defuzz_abs_event()
		 * reproduces stock's exact noise rejection without snapping to
		 * axes or clipping travel.  Honour a larger DT/boot.ini fuzz if
		 * one is set.  Trigger slots (>= 4) keep the DT fuzz.
		 */
		if (nbtn < 4 && fuzz < JOY_ABS_FUZZ)
			fuzz = JOY_ABS_FUZZ;

		input_set_abs_params(input, adc->report_type,
				adc->min, adc->max,
				fuzz,
				joypad->bt_adc_flat);
		dev_info(dev,
			"%s : SCALE = %d, ABS min = %d, max = %d,"
			" fuzz = %d, flat = %d, deadzone = %d\n",
			__func__, adc->scale, adc->min, adc->max,
			fuzz, joypad->bt_adc_flat,
			joypad->bt_adc_deadzone);
		dev_info(dev,
			"%s : adc tuning_p = %d, adc_tuning_n = %d\n\n",
			__func__, adc->tuning_p, adc->tuning_n);
	}

	/* GPIO key setup */
	__set_bit(EV_KEY, input->evbit);
	for(nbtn = 0; nbtn < joypad->bt_gpio_count; nbtn++) {
		struct bt_gpio *gpio = &joypad->gpios[nbtn];
		input_set_capability(input, gpio->report_type,
				gpio->linux_code);
	}

	if (joypad->auto_repeat)
		__set_bit(EV_REP, input->evbit);

	joypad->dev = dev;

	error = input_setup_polling(input, joypad_poll);
	if (error) {
		dev_err(dev, "unable to set up polling, err=%d\n", error);
		return error;
	}
	input_set_poll_interval(input, joypad->poll_interval);

	error = input_register_device(input);
	if (error) {
		dev_err(dev, "unable to register input device, err=%d\n",
			error);
		return error;
	}
	return 0;
}

/*----------------------------------------------------------------------------*/
static void joypad_setup_value_check(struct device *dev, struct joypad *joypad)
{
	/*
		fuzz: specifies fuzz value that is used to filter noise from
			the event stream.
	*/
	if (g_button_adc_fuzz)
		joypad->bt_adc_fuzz = g_button_adc_fuzz;
	else
		device_property_read_u32(dev, "button-adc-fuzz",
					&joypad->bt_adc_fuzz);
	/*
		flat: values that are within this value will be discarded by
			joydev interface and reported as 0 instead.
	*/
	if (g_button_adc_flat)
		joypad->bt_adc_flat = g_button_adc_flat;
	else
		device_property_read_u32(dev, "button-adc-flat",
					&joypad->bt_adc_flat);

	/* Joystick report value control */
	if (g_button_adc_scale)
		joypad->bt_adc_scale = g_button_adc_scale;
	else
		device_property_read_u32(dev, "button-adc-scale",
					&joypad->bt_adc_scale);

	/* Joystick deadzone value control */
	if (g_button_adc_deadzone)
		joypad->bt_adc_deadzone = g_button_adc_deadzone;
	else
		device_property_read_u32(dev, "button-adc-deadzone",
					&joypad->bt_adc_deadzone);

}

/*----------------------------------------------------------------------------*/
static int joypad_dt_parse(struct device *dev, struct joypad *joypad)
{
	int error = 0;

	/* initialize value check from boot.ini */
	joypad_setup_value_check(dev, joypad);

	device_property_read_u32(dev, "amux-count",
				&joypad->amux_count);

	device_property_read_u32(dev, "poll-interval",
				&joypad->poll_interval);

	joypad->auto_repeat = device_property_present(dev, "autorepeat");

	joypad->bt_gpio_count = device_get_child_node_count(dev);

	if ((joypad->amux_count == 0) || (joypad->bt_gpio_count == 0)) {
		dev_err(dev, "adc key = %d, gpio key = %d error!",
			joypad->amux_count, joypad->bt_gpio_count);
		return -EINVAL;
	}

	error = joypad_adc_setup(dev, joypad);
	if (error)
		return error;

	error = joypad_amux_setup(dev, joypad);
	if (error)
		return error;

	error = joypad_gpio_setup(dev, joypad);
	if (error)
		return error;

	dev_info(dev, "%s : adc key cnt = %d, gpio key cnt = %d\n",
			__func__, joypad->amux_count, joypad->bt_gpio_count);

	return error;
}

/*----------------------------------------------------------------------------*/
static int joypad_probe(struct platform_device *pdev)
{
	struct joypad *joypad;
	struct device *dev = &pdev->dev;
	int error;

	joypad = devm_kzalloc(dev, sizeof(struct joypad), GFP_KERNEL);
	if (!joypad) {
		dev_err(dev, "joypad devm_kzmalloc error!");
		return -ENOMEM;
	}

	/* device tree data parse */
	error = joypad_dt_parse(dev, joypad);
	if (error) {
		dev_err(dev, "dt parse error!(err = %d)\n", error);
		return error;
	}

	mutex_init(&joypad->lock);
	platform_set_drvdata(pdev, joypad);

	error = sysfs_create_group(&pdev->dev.kobj, &joypad_attr_group);
	if (error) {
		dev_err(dev, "create sysfs group fail, error: %d\n",
			error);
		return error;
	}

	/* poll input device setup */
	error = joypad_input_setup(dev, joypad);
	if (error) {
		dev_err(dev, "input setup failed!(err = %d)\n", error);
		return error;
	}
	dev_info(dev, "%s : probe success\n", __func__);
	return 0;
}

/*----------------------------------------------------------------------------*/
static const struct of_device_id joypad_of_match[] = {
	{ .compatible = "singleadc-joypad", },
	{},
};

MODULE_DEVICE_TABLE(of, joypad_of_match);

/*----------------------------------------------------------------------------*/
static struct platform_driver joypad_driver = {
	.probe = joypad_probe,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = of_match_ptr(joypad_of_match),
	},
};

/*----------------------------------------------------------------------------*/
static int __init joypad_init(void)
{
	return platform_driver_register(&joypad_driver);
}

/*----------------------------------------------------------------------------*/
static void __exit joypad_exit(void)
{
	platform_driver_unregister(&joypad_driver);
}

/*----------------------------------------------------------------------------*/
late_initcall(joypad_init);
module_exit(joypad_exit);
