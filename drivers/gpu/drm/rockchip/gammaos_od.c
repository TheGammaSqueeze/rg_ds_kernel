// SPDX-License-Identifier: GPL-2.0
/*
 * System wide panel overdrive for Rockchip VOP2 video ports.
 *
 * Liquid crystal does not finish a transition within one refresh, so moving
 * content leaves a trail. Overdrive drives each changing pixel past its target
 * for one refresh so that it arrives in time. Doing it here, on the frame the
 * display is about to scan out, covers every application at once.
 *
 * The per pixel work runs on the GPU in a userspace helper (the shader compiler
 * lives in userspace). For each eligible frame of an enabled video port the
 * display driver calls gammaos_od_frame() from its plane update: the frame's
 * buffer is handed to the helper through /dev/gammaos_od, the helper renders the
 * overdriven picture into one of the output buffers it registered, and the plane
 * scans out that buffer. A late or absent helper leaves the frame untouched, so
 * the feature can never stall or blank the display.
 *
 * The overdriven picture is meant for exactly one refresh. When no new frame is
 * committed for the next refresh (a static screen, or a game drawing below the
 * refresh rate), the frame start interrupt puts the application's own buffer
 * back, and the helper is told at the next frame how many refreshes the previous
 * picture stayed up, so its panel model stays in step.
 *
 * Eligible: the bottom plane of the port, linear 32-bit RGB (XRGB, ARGB, XBGR or
 * ABGR 8888) covering the whole screen with no scaling, offset, mirroring or
 * rotation. Anything else (AFBC buffers, overlays, video) passes through
 * unchanged.
 *
 * Control: /sys/class/misc/gammaos_od/vpN_enable (0/1), statistics in stats.
 */
#include <drm/drm_drv.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>
#include <linux/completion.h>
#include <linux/dma-buf.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <uapi/linux/gammaos_od.h>

#include "rockchip_drm_gem.h"
#include "gammaos_od.h"

#define OD_VPS		2
#define OD_SRCS		8
#define OD_TIMEOUT_MS	6

struct od_src {
	struct drm_gem_object *obj;	/* referenced while cached */
	struct dma_buf *buf;
	u64 id;
	u64 last_use;
	bool sent;
};

struct od_vp {
	bool enabled;
	bool ready;
	u32 width, height, pitch, count;
	struct dma_buf *out_buf[GAMMAOS_OD_MAX_OUT];
	struct drm_gem_object *out_obj[GAMMAOS_OD_MAX_OUT];
	dma_addr_t out_addr[GAMMAOS_OD_MAX_OUT];
	u32 next_out;

	/* the job handed to the helper */
	bool job_pending, job_taken;
	u32 seq;
	struct od_src *job_src;
	u32 job_out, job_pitch, job_held;
	int job_status;
	struct completion job_done;

	/* scanout tracking (frame start interrupt and plane update) */
	int pending_out;		/* programmed by the last commit, latches at the next frame start */
	dma_addr_t pending_orig;
	u32 pending_seq;
	int scanned;			/* output buffer on screen, -1 for the application's own */
	u32 commit_seq;			/* plane updates of the bottom plane */
	u32 held;			/* refreshes since the last helper frame */

	u64 frames, late, bypassed, reverts;
};

static struct od_vp od_vps[OD_VPS];
static struct od_src od_srcs[OD_SRCS];
static u64 od_next_id = 1, od_use_clock;
static u64 od_evict[GAMMAOS_OD_MAX_EVICT];
static struct drm_device *od_drm;
static bool od_helper;
static DEFINE_MUTEX(od_lock);		/* everything but the interrupt side */
static DEFINE_SPINLOCK(od_irq_lock);	/* scanout tracking, shared with the interrupt */
static DECLARE_WAIT_QUEUE_HEAD(od_wq);

void gammaos_od_set_drm(struct drm_device *drm)
{
	mutex_lock(&od_lock);
	od_drm = drm;
	mutex_unlock(&od_lock);
}

static void od_queue_evict(u64 id)
{
	int i;

	for (i = 0; i < GAMMAOS_OD_MAX_EVICT; i++)
		if (!od_evict[i]) {
			od_evict[i] = id;
			return;
		}
}

static bool od_src_busy(struct od_src *s)
{
	int v;

	for (v = 0; v < OD_VPS; v++)
		if (od_vps[v].job_src == s)
			return true;
	return false;
}

static void od_src_drop(struct od_src *s)
{
	if (!s->obj)
		return;
	if (s->sent)
		od_queue_evict(s->id);
	dma_buf_put(s->buf);
	drm_gem_object_put(s->obj);
	memset(s, 0, sizeof(*s));
}

/* find or export the dma-buf of a frame buffer object; od_lock held */
static struct od_src *od_src_get(struct drm_gem_object *obj)
{
	struct od_src *s, *victim = NULL;
	struct dma_buf *buf;
	int i;

	for (i = 0; i < OD_SRCS; i++) {
		s = &od_srcs[i];
		if (s->obj == obj) {
			s->last_use = ++od_use_clock;
			return s;
		}
	}
	for (i = 0; i < OD_SRCS; i++) {
		s = &od_srcs[i];
		if (!s->obj) {
			victim = s;
			break;
		}
		if (!od_src_busy(s) && (!victim || s->last_use < victim->last_use))
			victim = s;
	}
	if (!victim)
		return NULL;
	if (obj->dma_buf) {
		buf = obj->dma_buf;
		get_dma_buf(buf);
	} else {
		buf = drm_gem_prime_export(obj, O_RDWR);
		if (IS_ERR(buf))
			return NULL;
	}
	od_src_drop(victim);
	drm_gem_object_get(obj);
	victim->obj = obj;
	victim->buf = buf;
	victim->id = od_next_id++;
	victim->last_use = ++od_use_clock;
	victim->sent = false;
	return victim;
}

static bool od_fb_eligible(struct od_vp *v, struct drm_framebuffer *fb)
{
	u32 f = fb->format->format;

	/* 32-bit RGB in either byte order: the helper treats the three colour bytes
	 * alike and writes them back in place, and the plane keeps its format */
	if (f != DRM_FORMAT_XRGB8888 && f != DRM_FORMAT_ARGB8888 &&
	    f != DRM_FORMAT_XBGR8888 && f != DRM_FORMAT_ABGR8888)
		return false;
	if (fb->modifier != DRM_FORMAT_MOD_LINEAR || fb->offsets[0])
		return false;
	return fb->width == v->width && fb->height == v->height && fb->obj[0];
}

void gammaos_od_commit_begin(unsigned int vp)
{
	unsigned long flags;

	if (vp >= OD_VPS)
		return;
	spin_lock_irqsave(&od_irq_lock, flags);
	od_vps[vp].commit_seq++;
	od_vps[vp].pending_out = -1;
	spin_unlock_irqrestore(&od_irq_lock, flags);
}

bool gammaos_od_frame(unsigned int vp, struct drm_framebuffer *fb, dma_addr_t orig,
		      dma_addr_t *addr)
{
	struct od_vp *v;
	struct od_src *s;
	unsigned long flags;
	bool ok = false;
	u32 out, held, cseq;
	int i;

	if (vp >= OD_VPS)
		return false;
	v = &od_vps[vp];
	if (!READ_ONCE(v->enabled) || !READ_ONCE(od_helper))
		return false;

	mutex_lock(&od_lock);
	if (!v->ready || !od_helper || !od_fb_eligible(v, fb)) {
		v->bypassed++;
		goto out;
	}
	s = od_src_get(fb->obj[0]);
	if (!s) {
		v->bypassed++;
		goto out;
	}
	/*
	 * Round robin over the output buffers. Only one commit is in flight per port,
	 * so the buffer used two frames ago is off screen; with at least three buffers
	 * the next one in turn is always free. Skip the one on screen in any case.
	 */
	spin_lock_irqsave(&od_irq_lock, flags);
	out = v->next_out % v->count;
	for (i = 0; i < v->count && (out == v->scanned || out == v->pending_out); i++)
		out = (out + 1) % v->count;
	held = v->held ? v->held - 1 : 0;
	cseq = v->commit_seq;
	spin_unlock_irqrestore(&od_irq_lock, flags);

	v->seq++;
	v->job_src = s;
	v->job_out = out;
	v->job_pitch = fb->pitches[0];
	v->job_held = held;
	v->job_status = -ETIMEDOUT;
	v->job_taken = false;
	reinit_completion(&v->job_done);
	v->job_pending = true;
	mutex_unlock(&od_lock);
	wake_up_interruptible(&od_wq);

	wait_for_completion_timeout(&v->job_done, msecs_to_jiffies(OD_TIMEOUT_MS));

	mutex_lock(&od_lock);
	if (v->job_status == 0) {
		*addr = v->out_addr[out];
		v->next_out = out + 1;
		v->frames++;
		spin_lock_irqsave(&od_irq_lock, flags);
		if (v->commit_seq == cseq) {
			v->pending_out = out;
			v->pending_orig = orig;
			v->pending_seq = cseq;
			v->held = 0;
			ok = true;
		}
		spin_unlock_irqrestore(&od_irq_lock, flags);
	} else {
		v->late++;
	}
	v->job_pending = false;
	v->job_src = NULL;
out:
	mutex_unlock(&od_lock);
	return ok;
}

bool gammaos_od_vblank(unsigned int vp, dma_addr_t *revert)
{
	struct od_vp *v;
	bool r = false;

	if (vp >= OD_VPS)
		return false;
	v = &od_vps[vp];
	spin_lock(&od_irq_lock);
	v->held++;
	if (v->pending_out >= 0) {
		/* an overdriven frame just latched: unless a newer commit is already on
		 * its way, put the application's own buffer back for the next refresh */
		v->scanned = v->pending_out;
		v->pending_out = -1;
		if (v->commit_seq == v->pending_seq) {
			*revert = v->pending_orig;
			v->reverts++;
			r = true;
		}
	} else if (v->scanned >= 0) {
		/* the revert, or a newer frame of the application, is on screen now */
		v->scanned = -1;
	}
	spin_unlock(&od_irq_lock);
	return r;
}

/* ---- /dev/gammaos_od --------------------------------------------------- */

static void od_vp_clear(struct od_vp *v)
{
	int i;

	for (i = 0; i < GAMMAOS_OD_MAX_OUT; i++) {
		if (v->out_obj[i])
			drm_gem_object_put(v->out_obj[i]);
		if (v->out_buf[i])
			dma_buf_put(v->out_buf[i]);
		v->out_obj[i] = NULL;
		v->out_buf[i] = NULL;
		v->out_addr[i] = 0;
	}
	v->ready = false;
}

static long od_setup(void __user *arg)
{
	struct gammaos_od_setup st;
	struct od_vp *v;
	unsigned long flags;
	long ret = 0;
	u32 i;

	if (copy_from_user(&st, arg, sizeof(st)))
		return -EFAULT;
	if (st.vp >= OD_VPS || st.count < 2 || st.count > GAMMAOS_OD_MAX_OUT ||
	    !st.width || !st.height || st.pitch < st.width * 4)
		return -EINVAL;
	mutex_lock(&od_lock);
	v = &od_vps[st.vp];
	if (!od_drm) {
		ret = -ENODEV;
		goto out;
	}
	spin_lock_irqsave(&od_irq_lock, flags);
	if (v->scanned >= 0 || v->pending_out >= 0)
		ret = -EBUSY;	/* an old output buffer is still on screen */
	spin_unlock_irqrestore(&od_irq_lock, flags);
	if (ret || v->job_pending)
		goto out_busy;
	od_vp_clear(v);
	for (i = 0; i < st.count; i++) {
		struct dma_buf *buf = dma_buf_get(st.fds[i]);
		struct drm_gem_object *obj;

		if (IS_ERR(buf)) {
			ret = PTR_ERR(buf);
			break;
		}
		obj = drm_gem_prime_import(od_drm, buf);
		if (IS_ERR(obj)) {
			dma_buf_put(buf);
			ret = PTR_ERR(obj);
			break;
		}
		v->out_buf[i] = buf;
		v->out_obj[i] = obj;
		v->out_addr[i] = to_rockchip_obj(obj)->dma_addr;
		if (!v->out_addr[i] || obj->size < (size_t)st.pitch * st.height) {
			ret = -EINVAL;
			break;
		}
	}
	if (ret) {
		od_vp_clear(v);
		goto out;
	}
	v->width = st.width;
	v->height = st.height;
	v->pitch = st.pitch;
	v->count = st.count;
	v->next_out = 0;
	v->ready = true;
	goto out;
out_busy:
	if (!ret)
		ret = -EBUSY;
out:
	mutex_unlock(&od_lock);
	return ret;
}

static bool od_job_ready(void)
{
	int i;

	for (i = 0; i < OD_VPS; i++)
		if (od_vps[i].job_pending && !od_vps[i].job_taken)
			return true;
	return false;
}

static long od_wait(void __user *arg)
{
	struct gammaos_od_job job;
	struct od_vp *v = NULL;
	int i, ret;

	ret = wait_event_interruptible(od_wq, od_job_ready());
	if (ret)
		return ret;
	mutex_lock(&od_lock);
	for (i = 0; i < OD_VPS; i++)
		if (od_vps[i].job_pending && !od_vps[i].job_taken) {
			v = &od_vps[i];
			break;
		}
	if (!v) {
		mutex_unlock(&od_lock);
		return -EAGAIN;
	}
	memset(&job, 0, sizeof(job));
	job.vp = i;
	job.seq = v->seq;
	job.src_id = v->job_src->id;
	job.src_pitch = v->job_pitch;
	job.out_index = v->job_out;
	job.held_frames = v->job_held;
	job.src_fd = -1;
	if (!v->job_src->sent) {
		get_dma_buf(v->job_src->buf);
		job.src_fd = dma_buf_fd(v->job_src->buf, O_CLOEXEC);
		if (job.src_fd < 0)
			dma_buf_put(v->job_src->buf);
		else
			v->job_src->sent = true;
	}
	memcpy(job.evict, od_evict, sizeof(od_evict));
	memset(od_evict, 0, sizeof(od_evict));
	v->job_taken = true;
	mutex_unlock(&od_lock);
	if (copy_to_user(arg, &job, sizeof(job)))
		return -EFAULT;
	return 0;
}

static long od_done(void __user *arg)
{
	struct gammaos_od_done d;
	struct od_vp *v;

	if (copy_from_user(&d, arg, sizeof(d)))
		return -EFAULT;
	if (d.vp >= OD_VPS)
		return -EINVAL;
	mutex_lock(&od_lock);
	v = &od_vps[d.vp];
	if (v->job_pending && v->job_taken && v->seq == d.seq) {
		v->job_status = d.status;
		complete(&v->job_done);
	}
	mutex_unlock(&od_lock);
	return 0;
}

static long od_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case GAMMAOS_OD_SETUP:
		return od_setup((void __user *)arg);
	case GAMMAOS_OD_WAIT:
		return od_wait((void __user *)arg);
	case GAMMAOS_OD_DONE:
		return od_done((void __user *)arg);
	}
	return -ENOTTY;
}

static int od_open(struct inode *inode, struct file *f)
{
	int ret = 0, i;

	mutex_lock(&od_lock);
	if (od_helper) {
		ret = -EBUSY;
	} else {
		od_helper = true;
		/* a new helper has no cached sources: send every fd again */
		for (i = 0; i < OD_SRCS; i++)
			od_srcs[i].sent = false;
		memset(od_evict, 0, sizeof(od_evict));
	}
	mutex_unlock(&od_lock);
	return ret;
}

static int od_release(struct inode *inode, struct file *f)
{
	int i;

	mutex_lock(&od_lock);
	od_helper = false;
	for (i = 0; i < OD_VPS; i++) {
		od_vps[i].ready = false;
		if (od_vps[i].job_pending) {
			od_vps[i].job_status = -EPIPE;
			complete(&od_vps[i].job_done);
		}
	}
	/* output buffers stay mapped until the next setup: one may still be on screen */
	mutex_unlock(&od_lock);
	return 0;
}

static const struct file_operations od_fops = {
	.owner = THIS_MODULE,
	.open = od_open,
	.release = od_release,
	.unlocked_ioctl = od_ioctl,
	.compat_ioctl = od_ioctl,
};

static ssize_t od_enable_show(struct device *dev, char *buf, int vp)
{
	return sysfs_emit(buf, "%d\n", od_vps[vp].enabled);
}

static ssize_t od_enable_store(struct device *dev, const char *buf, size_t n, int vp)
{
	bool on;

	if (kstrtobool(buf, &on))
		return -EINVAL;
	WRITE_ONCE(od_vps[vp].enabled, on);
	return n;
}

static ssize_t vp0_enable_show(struct device *d, struct device_attribute *a, char *b)
{
	return od_enable_show(d, b, 0);
}
static ssize_t vp0_enable_store(struct device *d, struct device_attribute *a, const char *b, size_t n)
{
	return od_enable_store(d, b, n, 0);
}
static DEVICE_ATTR_RW(vp0_enable);

static ssize_t vp1_enable_show(struct device *d, struct device_attribute *a, char *b)
{
	return od_enable_show(d, b, 1);
}
static ssize_t vp1_enable_store(struct device *d, struct device_attribute *a, const char *b, size_t n)
{
	return od_enable_store(d, b, n, 1);
}
static DEVICE_ATTR_RW(vp1_enable);

static ssize_t stats_show(struct device *d, struct device_attribute *a, char *buf)
{
	ssize_t n = 0;
	int i;

	n += sysfs_emit_at(buf, n, "helper=%d\n", od_helper);
	for (i = 0; i < OD_VPS; i++) {
		struct od_vp *v = &od_vps[i];

		n += sysfs_emit_at(buf, n,
				   "vp%d enabled=%d ready=%d %ux%u frames=%llu late=%llu bypassed=%llu reverts=%llu scanned=%d\n",
				   i, v->enabled, v->ready, v->width, v->height, v->frames, v->late,
				   v->bypassed, v->reverts, v->scanned);
	}
	return n;
}
static DEVICE_ATTR_RO(stats);

static struct attribute *od_attrs[] = {
	&dev_attr_vp0_enable.attr,
	&dev_attr_vp1_enable.attr,
	&dev_attr_stats.attr,
	NULL,
};
ATTRIBUTE_GROUPS(od);

static struct miscdevice od_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "gammaos_od",
	.fops = &od_fops,
	.groups = od_groups,
	.mode = 0600,
};

static int __init gammaos_od_init(void)
{
	int i;

	for (i = 0; i < OD_VPS; i++) {
		init_completion(&od_vps[i].job_done);
		od_vps[i].pending_out = -1;
		od_vps[i].scanned = -1;
	}
	return misc_register(&od_misc);
}
late_initcall(gammaos_od_init);
