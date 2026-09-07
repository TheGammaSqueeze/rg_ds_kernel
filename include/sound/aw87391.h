/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * aw87391.h -- AW87391 speaker-PA hooks for the RG DS.
 *
 * The stock RG DS DTB does not wire the two AW87391 PAs into the ASoC card as
 * aux-devs, so there is no DAPM event to gate them.  Stock instead drives the
 * PAs straight from the RK817 codec's mute_stream callback (enable LAST on the
 * speaker unmute, disable FIRST on mute).  These hooks let rk817_codec.c do the
 * same, which is what avoids the cold-boot / resume enable-into-idle-DAC pop.
 */
#ifndef __SOUND_AW87391_H__
#define __SOUND_AW87391_H__

#if IS_ENABLED(CONFIG_SND_SOC_AW87391)
void aw87391_speakers_enable(void);
void aw87391_speakers_disable(void);
#else
static inline void aw87391_speakers_enable(void) { }
static inline void aw87391_speakers_disable(void) { }
#endif

#endif /* __SOUND_AW87391_H__ */
