/* SPDX-License-Identifier: GPL-2.0 */
/* System wide panel overdrive: display driver side, see gammaos_od.c */
#ifndef _GAMMAOS_OD_H
#define _GAMMAOS_OD_H

#include <linux/types.h>

struct drm_device;
struct drm_framebuffer;

#if IS_ENABLED(CONFIG_GAMMAOS_OD)
void gammaos_od_set_drm(struct drm_device *drm);
/* the bottom plane of @vp is being updated (any frame, eligible or not) */
void gammaos_od_commit_begin(unsigned int vp);
/* overdrive this frame; on success *addr is the buffer to scan out instead of @orig */
bool gammaos_od_frame(unsigned int vp, struct drm_framebuffer *fb, dma_addr_t orig,
		      dma_addr_t *addr);
/* frame start of @vp; true when the application's buffer *revert must go back */
bool gammaos_od_vblank(unsigned int vp, dma_addr_t *revert);
#else
static inline void gammaos_od_set_drm(struct drm_device *drm) {}
static inline void gammaos_od_commit_begin(unsigned int vp) {}
static inline bool gammaos_od_frame(unsigned int vp, struct drm_framebuffer *fb,
				    dma_addr_t orig, dma_addr_t *addr)
{
	return false;
}
static inline bool gammaos_od_vblank(unsigned int vp, dma_addr_t *revert)
{
	return false;
}
#endif

#endif
