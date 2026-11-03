/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Panel overdrive helper interface (/dev/gammaos_od).
 *
 * The display driver hands each eligible frame of a video port to a userspace
 * helper, which renders the overdriven picture into one of the output buffers it
 * registered, then reports back. The display then scans out that buffer instead of
 * the application's frame. If the helper is late or absent, the application's
 * frame is shown unchanged.
 */
#ifndef _UAPI_LINUX_GAMMAOS_OD_H
#define _UAPI_LINUX_GAMMAOS_OD_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define GAMMAOS_OD_MAX_OUT	3
#define GAMMAOS_OD_MAX_EVICT	4

/* register the output buffers of one video port (linear XRGB8888) */
struct gammaos_od_setup {
	__u32 vp;
	__u32 width;
	__u32 height;
	__u32 pitch;
	__u32 count;			/* 2..GAMMAOS_OD_MAX_OUT */
	__s32 fds[GAMMAOS_OD_MAX_OUT];	/* dma-buf fds, kept by the kernel */
};

/* a frame to process, returned by GAMMAOS_OD_WAIT */
struct gammaos_od_job {
	__u32 vp;
	__u32 seq;
	__u64 src_id;			/* stable id of the source buffer */
	__s32 src_fd;			/* dma-buf fd the first time src_id is seen, else -1 */
	__u32 src_pitch;
	__u32 out_index;		/* render into this output buffer */
	__u32 held_frames;		/* refreshes the previous frame stayed on screen beyond the first */
	__u64 evict[GAMMAOS_OD_MAX_EVICT];	/* source ids the helper may forget (0 = none) */
};

/* the job is rendered (status 0) or could not be (status < 0) */
struct gammaos_od_done {
	__u32 vp;
	__u32 seq;
	__s32 status;
	__u32 pad;
};

#define GAMMAOS_OD_SETUP	_IOW('G', 1, struct gammaos_od_setup)
#define GAMMAOS_OD_WAIT		_IOR('G', 2, struct gammaos_od_job)
#define GAMMAOS_OD_DONE		_IOW('G', 3, struct gammaos_od_done)

#endif /* _UAPI_LINUX_GAMMAOS_OD_H */
