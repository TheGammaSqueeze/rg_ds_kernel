/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Backlight black frame insertion (BFI) for PWM backlights, locked to the
 * display controller's frame start.
 */
#ifndef _LINUX_GAMMAOS_BFI_H
#define _LINUX_GAMMAOS_BFI_H

#include <linux/types.h>

struct backlight_device;
struct pwm_device;
struct gammaos_bfi;

#if IS_ENABLED(CONFIG_GAMMAOS_BFI)
/* pwm_bl side */
struct gammaos_bfi *gammaos_bfi_attach(struct backlight_device *bl,
				       struct pwm_device *pwm, u32 vp);
void gammaos_bfi_detach(struct gammaos_bfi *bfi);
bool gammaos_bfi_enabled(struct gammaos_bfi *bfi);
/* brightness as a fraction of full scale, 0..1024; 0 hands the PWM back */
void gammaos_bfi_set_level(struct gammaos_bfi *bfi, u32 level);

/* display side: called from the frame start interrupt of video port @vp */
void gammaos_bfi_vblank(unsigned int vp);
/* the display driver lends a way to keep a video port's frame interrupt on */
void gammaos_bfi_set_vblank_ctl(int (*get)(unsigned int vp),
				void (*put)(unsigned int vp));
#else
static inline struct gammaos_bfi *gammaos_bfi_attach(struct backlight_device *bl,
						     struct pwm_device *pwm, u32 vp)
{
	return NULL;
}
static inline void gammaos_bfi_detach(struct gammaos_bfi *bfi) {}
static inline bool gammaos_bfi_enabled(struct gammaos_bfi *bfi) { return false; }
static inline void gammaos_bfi_set_level(struct gammaos_bfi *bfi, u32 level) {}
static inline void gammaos_bfi_vblank(unsigned int vp) {}
static inline void gammaos_bfi_set_vblank_ctl(int (*get)(unsigned int vp),
					      void (*put)(unsigned int vp)) {}
#endif

#endif /* _LINUX_GAMMAOS_BFI_H */
