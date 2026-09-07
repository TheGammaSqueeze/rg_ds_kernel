// SPDX-License-Identifier: GPL-2.0-only
//
// aw87391.c  --  AW87391 speaker amplifier driver (RG DS)
//
// Based on the Armbian RG DS driver (board-rgds-01-asoc-aw87391), adapted for
// the stock Anbernic device tree.  The RG DS drives its two speakers through a
// pair of AW87391 smart PAs (DT: "aw,aw87391-left" @0x58, "aw,aw87391-right"
// @0x5b on i2c@fe5b0000).  Without a bound driver the PAs stay at their chip
// default (0x01 bit0 clear = disabled), so only headphones (straight off the
// RK817 codec) produce sound and the speakers are silent.
//
// The Armbian variant enables/disables the PA from an ASoC DAPM widget wired
// into the card via simple-audio-card,aux-devs.  The stock Anbernic DTB does
// NOT reference the PAs from its rockchip,multicodecs-card node (it only lists
// the RK817), so the DAPM path never fires here.  To work against the
// unmodified stock DTB we also power the PA up at probe (and re-apply on
// resume), which reproduces the stock kernel's behaviour.  The register values
// are the exact set the stock RG DS kernel leaves the live chip in.
//

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <sound/soc.h>
#include <sound/aw87391.h>

struct aw87391_regval {
	u8 reg;
	u8 val;
};

struct aw87391_priv {
	struct regmap *regmap;
	struct gpio_desc *enable_gpiod;
	struct regulator *vcc;
	bool initialized;
	bool powered;
};

/*
 * The two PAs are plain i2c devices on the stock DTB (not ASoC aux-devs), so the
 * RK817 codec reaches them by side through this registry and drives them from its
 * mute_stream callback, exactly as the stock kernel does.
 */
static struct aw87391_priv *aw87391_pa_left;
static struct aw87391_priv *aw87391_pa_right;

/*
 * Gain / config captured from the stock RG DS kernel's live PA state
 * (i2cget -f -y 2 0x58/0x5b): 0x03=01, 0x04=45, 0x05=4e, 0x06=4a, 0x07=4a.
 * 0x02 is left at the chip default (0x58), matching stock.
 */
/*
 * Stock RG DS speaker-on (kspk) profile, decompiled 1:1 from the stock kernel's
 * aw87391_kspk_reg and written in order on every power-up.  The trailing staged
 * enable (0x03=0x00, then 0x01=0x07 -> 0x01=0x3f, then 0x03=0x01) together with
 * the boost / DFT trim registers (0x5d..0x7d) is the ANTI-POP sequence: bring
 * the boost up muted, soft-enable the PA, then full-enable and raise the boost.
 * Our old one-shot 0x01=0x3f skipped all of this, which is why enabling from an
 * off state popped.
 */
static const struct aw87391_regval aw87391_on_reg[] = {
	{ 0x64, 0x3a }, { 0x02, 0x58 }, { 0x04, 0x45 }, { 0x05, 0x4e },
	{ 0x5d, 0x00 }, { 0x5e, 0xb4 }, { 0x5f, 0x30 }, { 0x60, 0x39 },
	{ 0x61, 0x10 }, { 0x62, 0x03 }, { 0x63, 0x7d }, { 0x65, 0xa0 },
	{ 0x66, 0x21 }, { 0x67, 0x41 }, { 0x68, 0x3b }, { 0x6e, 0x00 },
	{ 0x6f, 0x00 }, { 0x70, 0x00 }, { 0x71, 0x00 }, { 0x72, 0x34 },
	{ 0x73, 0x06 }, { 0x74, 0x10 }, { 0x75, 0x00 }, { 0x7a, 0x00 },
	{ 0x7b, 0x00 }, { 0x7c, 0x00 }, { 0x7d, 0x00 }, { 0x03, 0x00 },
	{ 0x01, 0x07 }, { 0x01, 0x3f }, { 0x03, 0x01 },
};

static const struct aw87391_regval aw87391_off_reg[] = {
	{ 0x01, 0x00 },
};

static const struct regmap_config aw87391_regmap_config = {
	.val_bits = 8,
	.reg_bits = 8,
	.max_register = 0x7f,
	.cache_type = REGCACHE_NONE,
};

static int aw87391_apply_seq(struct aw87391_priv *aw,
			     const struct aw87391_regval *seq,
			     size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = regmap_write(aw->regmap, seq[i].reg, seq[i].val);
		if (ret)
			return ret;
	}

	return 0;
}

static int aw87391_pa_enable(struct aw87391_priv *aw)
{
	int ret;

	if (!aw)
		return -ENODEV;

	if (!aw->powered) {
		if (aw->vcc) {
			ret = regulator_enable(aw->vcc);
			if (ret)
				return ret;
		}

		if (aw->enable_gpiod)
			gpiod_set_value_cansleep(aw->enable_gpiod, 1);

		if (aw->vcc || aw->enable_gpiod)
			usleep_range(1000, 2000);
	}

	/*
	 * Write the full stock speaker-on profile (config + staged anti-pop
	 * enable) on every stream start, exactly as stock's enable_pa_spk_*.
	 */
	ret = aw87391_apply_seq(aw, aw87391_on_reg,
				ARRAY_SIZE(aw87391_on_reg));
	if (ret)
		goto err_power;

	/*
	 * Stock enable_pa_spk_* waits 50 ms (50 x udelay(1000)) after writing the
	 * kspk profile so the boost/PA settle before audio reaches it.  This runs
	 * from the codec mute_stream callback (sleepable process context).
	 */
	msleep(50);

	aw->initialized = true;

	aw->powered = true;

	return 0;

err_power:
	if (aw->enable_gpiod) {
		gpiod_set_value_cansleep(aw->enable_gpiod, 0);
		usleep_range(1000, 2000);
	}

	if (aw->vcc)
		regulator_disable(aw->vcc);

	aw->initialized = false;

	return ret;
}

static int aw87391_pa_disable(struct aw87391_priv *aw)
{
	int ret;

	if (!aw || !aw->powered)
		return 0;

	/* Best-effort: still drop power even if the OFF write fails, so the
	 * shared vcc_amp rail is never left powered through suspend/shutdown. */
	ret = aw87391_apply_seq(aw, aw87391_off_reg,
				ARRAY_SIZE(aw87391_off_reg));

	if (aw->enable_gpiod) {
		gpiod_set_value_cansleep(aw->enable_gpiod, 0);
		usleep_range(1000, 2000);
	}

	if (aw->vcc)
		regulator_disable(aw->vcc);

	if (aw->enable_gpiod || aw->vcc)
		aw->initialized = false;

	aw->powered = false;

	return ret;
}

/*
 * Entry points for the RK817 codec's mute_stream callback: enable the speaker
 * PAs LAST on the speaker unmute (after the DAC is unmuted and the spk gpio is
 * raised) and disable them FIRST on mute -- the stock rk817_digital_mute
 * ordering, which (together with the 50 ms settle) is what avoids the pop.
 * Stock does right then left; order is immaterial (two independent i2c devices).
 */
void aw87391_speakers_enable(void)
{
	aw87391_pa_enable(aw87391_pa_right);
	aw87391_pa_enable(aw87391_pa_left);
}
EXPORT_SYMBOL_GPL(aw87391_speakers_enable);

void aw87391_speakers_disable(void)
{
	aw87391_pa_disable(aw87391_pa_right);
	aw87391_pa_disable(aw87391_pa_left);
}
EXPORT_SYMBOL_GPL(aw87391_speakers_disable);

static int aw87391_suspend(struct device *dev)
{
	struct aw87391_priv *aw = dev_get_drvdata(dev);

	aw87391_pa_disable(aw);
	return 0;
}

static int aw87391_resume(struct device *dev)
{
	/*
	 * Deliberately do NOT re-enable here.  Stock re-arms the PA from
	 * rk817_digital_mute on the next stream, never from PM resume; enabling
	 * into an idle/unsettled DAC on resume is exactly the resume pop.
	 */
	return 0;
}

static void aw87391_shutdown(struct i2c_client *i2c)
{
	struct aw87391_priv *aw = i2c_get_clientdata(i2c);

	if (!aw)
		return;

	aw87391_pa_disable(aw);
}

static SIMPLE_DEV_PM_OPS(aw87391_pm_ops, aw87391_suspend, aw87391_resume);

static int aw87391_drv_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct aw87391_priv *aw = snd_soc_component_get_drvdata(component);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		return aw87391_pa_enable(aw);
	case SND_SOC_DAPM_POST_PMD:
		aw87391_pa_disable(aw);
		return 0;
	default:
		return 0;
	}
}

static const struct snd_soc_dapm_widget aw87391_dapm_widgets[] = {
	SND_SOC_DAPM_INPUT("IN"),
	SND_SOC_DAPM_PGA_E("SPK PA", SND_SOC_NOPM, 0, 0, NULL, 0,
			   aw87391_drv_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUTPUT("OUT"),
};

static const struct snd_soc_dapm_route aw87391_dapm_routes[] = {
	{ "SPK PA", NULL, "IN" },
	{ "OUT", NULL, "SPK PA" },
};

static const struct snd_soc_component_driver aw87391_component_driver = {
	.dapm_widgets = aw87391_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(aw87391_dapm_widgets),
	.dapm_routes = aw87391_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(aw87391_dapm_routes),
};

static int aw87391_i2c_probe(struct i2c_client *i2c)
{
	struct aw87391_priv *aw;
	int ret;

	aw = devm_kzalloc(&i2c->dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;

	aw->regmap = devm_regmap_init_i2c(i2c, &aw87391_regmap_config);
	if (IS_ERR(aw->regmap))
		return dev_err_probe(&i2c->dev, PTR_ERR(aw->regmap),
				     "failed to init regmap\n");

	aw->enable_gpiod = devm_gpiod_get_optional(&i2c->dev, "enable",
						   GPIOD_OUT_LOW);
	if (IS_ERR(aw->enable_gpiod))
		return dev_err_probe(&i2c->dev, PTR_ERR(aw->enable_gpiod),
				     "failed to get enable gpio\n");

	aw->vcc = devm_regulator_get_optional(&i2c->dev, "vcc");
	if (IS_ERR(aw->vcc)) {
		if (PTR_ERR(aw->vcc) == -ENODEV)
			aw->vcc = NULL;
		else
			return dev_err_probe(&i2c->dev, PTR_ERR(aw->vcc),
					     "failed to get vcc regulator\n");
	}

	i2c_set_clientdata(i2c, aw);

	ret = devm_snd_soc_register_component(&i2c->dev,
					      &aw87391_component_driver,
					      NULL, 0);
	if (ret)
		return ret;

	/*
	 * Do NOT power the amp up at probe: the RK817 DAC is still idle this early
	 * (cold boot), and enabling into an unsettled DAC is the classic cold-start
	 * pop.  Instead register into the side registry so the RK817 codec can arm
	 * the PA from its mute_stream callback on the next stream start (after the
	 * DAC is unmuted), exactly as the stock kernel does.
	 */
	if (of_device_is_compatible(i2c->dev.of_node, "aw,aw87391-right"))
		aw87391_pa_right = aw;
	else
		aw87391_pa_left = aw;	/* "aw,aw87391-left" and the generic id */

	dev_info(&i2c->dev, "AW87391 speaker PA registered (%s)\n",
		 aw87391_pa_right == aw ? "right" : "left");

	return 0;
}

static const struct i2c_device_id aw87391_i2c_id[] = {
	{ "aw87391" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw87391_i2c_id);

static const struct of_device_id aw87391_of_match[] = {
	{ .compatible = "aw,aw87391-left" },
	{ .compatible = "aw,aw87391-right" },
	{ .compatible = "awinic,aw87391" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw87391_of_match);

static struct i2c_driver aw87391_i2c_driver = {
	.driver = {
		.name = "aw87391",
		.of_match_table = aw87391_of_match,
		.pm = &aw87391_pm_ops,
	},
	.probe_new = aw87391_i2c_probe,
	.shutdown = aw87391_shutdown,
	.id_table = aw87391_i2c_id,
};
module_i2c_driver(aw87391_i2c_driver);

MODULE_DESCRIPTION("ASoC AW87391 PA Driver (RG DS)");
MODULE_LICENSE("GPL v2");
