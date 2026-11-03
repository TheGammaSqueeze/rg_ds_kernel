// SPDX-License-Identifier: GPL-2.0
/*
 * gammaos_odd: GPU helper for the kernel's system wide panel overdrive.
 *
 * The display driver (drivers/gpu/drm/rockchip/gammaos_od.c) hands each eligible
 * frame of an enabled video port to this helper through /dev/gammaos_od. The
 * helper runs one Vulkan compute pass that writes the overdriven picture into one
 * of its output buffers and reports back; the display scans that buffer out.
 *
 * Every buffer is a dma-buf imported straight into Vulkan memory
 * (VK_EXT_external_memory_dma_buf) and bound as a storage buffer of XRGB8888
 * pixels: the application's frames, the previous frame and the output buffers.
 * The panel model's per-pixel estimate lives in a device buffer of half floats.
 * See od.comp for the model.
 *
 * Usage: gammaos_odd <profile file> <vp>:<width>x<height> [<vp>:<width>x<height>]
 * Profile file (one line, reread when it changes):
 *   rise_from_black rise_from_white curve fall threshold cap_up cap_down
 *   alphas in percent per refresh, curve 16 = linear, threshold and caps in 8-bit levels
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <android/log.h>
#include "gammaos_od.h"	/* copied from include/uapi/linux by build.sh */
#include "od_comp_spv.h"

#define TAG "gammaos_odd"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define MAX_SRC 16

struct profile {
	float rise_black, rise_white, curve, fall, thr, cap_up, cap_dn;
};

struct vbuf {
	VkBuffer buf;
	VkDeviceMemory mem;
	VkDeviceSize size;
};

struct src_buf {
	uint64_t id;
	unsigned pitch;		/* pixels */
	struct vbuf b;
};

struct port {
	unsigned vp, w, h;
	struct vbuf out[GAMMAOS_OD_MAX_OUT];
	struct vbuf state;
	struct src_buf src[MAX_SRC];
	uint64_t last_src;
	int primed;
	VkDescriptorSet set;
	VkCommandBuffer cmd;
};

struct push {
	uint32_t w, h, src_pitch, prev_pitch, out_pitch;
	float held, init, pad;
	float p0[4], p1[4];
};

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkPipelineLayout playout;
static VkPipeline od_pipe;
static VkDescriptorSetLayout dslayout;
static VkDescriptorPool dpool;
static VkCommandPool cpool;
static VkFence fence;
static PFN_vkGetMemoryFdPropertiesKHR p_fd_props;
static struct profile prof = { 90, 74, 22, 74, 8, 96, 96 };

#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { LOGE("%s failed: %d", #x, r_); return -1; } } while (0)

static int load_profile(const char *path, time_t *mt)
{
	struct stat st;
	struct profile p;
	FILE *f;

	if (stat(path, &st) || st.st_mtime == *mt)
		return 0;
	*mt = st.st_mtime;
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (fscanf(f, "%f %f %f %f %f %f %f", &p.rise_black, &p.rise_white, &p.curve, &p.fall,
		   &p.thr, &p.cap_up, &p.cap_dn) == 7) {
		prof = p;
		LOGI("profile: rise %.0f..%.0f curve %.0f fall %.0f thr %.0f caps %.0f/%.0f",
		     p.rise_black, p.rise_white, p.curve, p.fall, p.thr, p.cap_up, p.cap_dn);
	}
	fclose(f);
	return 1;
}

static int heap_alloc(size_t size)
{
	struct dma_heap_allocation_data a = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
	int heap = open("/dev/dma_heap/system-uncached", O_RDONLY | O_CLOEXEC);

	if (heap < 0)
		heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
	if (heap < 0)
		return -1;
	if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a)) {
		close(heap);
		return -1;
	}
	close(heap);
	return (int)a.fd;
}

static int mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	uint32_t i;

	vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	for (i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return (int)i;
	for (i = 0; i < mp.memoryTypeCount; i++)
		if (bits & (1u << i))
			return (int)i;
	return -1;
}

/* bind a dma-buf as a storage buffer; Vulkan takes ownership of fd on success */
static int import_dmabuf(int fd, struct vbuf *b)
{
	VkExternalMemoryBufferCreateInfo ext = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkBufferCreateInfo bi = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ext,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkMemoryFdPropertiesKHR fp = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
	VkImportMemoryFdInfoKHR imp = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd,
	};
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp };
	VkMemoryRequirements mr;
	off_t size = lseek(fd, 0, SEEK_END);
	int t;

	if (size <= 0)
		return -1;
	lseek(fd, 0, SEEK_SET);
	bi.size = (VkDeviceSize)size;
	VK(vkCreateBuffer(dev, &bi, NULL, &b->buf));
	vkGetBufferMemoryRequirements(dev, b->buf, &mr);
	VK(p_fd_props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp));
	t = mem_type(mr.memoryTypeBits & fp.memoryTypeBits, 0);
	if (t < 0)
		return -1;
	ai.allocationSize = mr.size > (VkDeviceSize)size ? mr.size : (VkDeviceSize)size;
	ai.memoryTypeIndex = (uint32_t)t;
	VK(vkAllocateMemory(dev, &ai, NULL, &b->mem));
	VK(vkBindBufferMemory(dev, b->buf, b->mem, 0));
	b->size = (VkDeviceSize)size;
	return 0;
}

static void vbuf_free(struct vbuf *b)
{
	if (b->buf)
		vkDestroyBuffer(dev, b->buf, NULL);
	if (b->mem)
		vkFreeMemory(dev, b->mem, NULL);
	memset(b, 0, sizeof(*b));
}

static int device_buffer(VkDeviceSize size, struct vbuf *b)
{
	VkBufferCreateInfo bi = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkMemoryRequirements mr;
	int t;

	VK(vkCreateBuffer(dev, &bi, NULL, &b->buf));
	vkGetBufferMemoryRequirements(dev, b->buf, &mr);
	t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (t < 0)
		return -1;
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = (uint32_t)t;
	VK(vkAllocateMemory(dev, &ai, NULL, &b->mem));
	VK(vkBindBufferMemory(dev, b->buf, b->mem, 0));
	b->size = size;
	return 0;
}

static int vk_init(void)
{
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	const char *dexts[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf" };
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
					.pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
				   .pQueueCreateInfos = &qci, .enabledExtensionCount = 2,
				   .ppEnabledExtensionNames = dexts };
	VkQueueFamilyProperties qf[8];
	uint32_t n = 1, nq = 8, i;
	VkDescriptorSetLayoutBinding binds[4];
	VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
					       .bindingCount = 4, .pBindings = binds };
	VkPushConstantRange pr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(struct push) };
	VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
					  .pSetLayouts = &dslayout, .pushConstantRangeCount = 1,
					  .pPushConstantRanges = &pr };
	VkShaderModuleCreateInfo sm = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
					.codeSize = sizeof(od_comp_spv), .pCode = od_comp_spv };
	VkShaderModule mod;
	VkComputePipelineCreateInfo cp = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 8 };
	VkDescriptorPoolCreateInfo dp = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2,
					  .poolSizeCount = 1, .pPoolSizes = &ps };
	VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
					.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
	VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

	VK(vkCreateInstance(&ici, NULL, &inst));
	VK(vkEnumeratePhysicalDevices(inst, &n, &phys));
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
	for (i = 0; i < nq; i++)
		if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
			break;
	if (i == nq)
		return -1;
	qfam = i;
	qci.queueFamilyIndex = qfam;
	VK(vkCreateDevice(phys, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, qfam, 0, &queue);
	p_fd_props = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
	if (!p_fd_props)
		return -1;
	for (i = 0; i < 4; i++)
		binds[i] = (VkDescriptorSetLayoutBinding){ .binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
							   .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
	VK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &dslayout));
	VK(vkCreatePipelineLayout(dev, &pl, NULL, &playout));
	VK(vkCreateShaderModule(dev, &sm, NULL, &mod));
	cp.stage = (VkPipelineShaderStageCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
						      .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main" };
	cp.layout = playout;
	VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, NULL, &od_pipe));
	VK(vkCreateDescriptorPool(dev, &dp, NULL, &dpool));
	cpi.queueFamilyIndex = qfam;
	VK(vkCreateCommandPool(dev, &cpi, NULL, &cpool));
	VK(vkCreateFence(dev, &fi, NULL, &fence));
	return 0;
}

static int port_setup(int odfd, struct port *p)
{
	struct gammaos_od_setup st = { .vp = p->vp, .width = p->w, .height = p->h,
				       .pitch = p->w * 4, .count = GAMMAOS_OD_MAX_OUT };
	VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
					    .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dslayout };
	VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
					    .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
					    .commandBufferCount = 1 };
	int i;

	if (device_buffer((VkDeviceSize)p->w * p->h * 8, &p->state))
		return -1;
	for (i = 0; i < GAMMAOS_OD_MAX_OUT; i++) {
		int fd = heap_alloc((size_t)p->w * 4 * p->h), kfd;

		if (fd < 0) {
			LOGE("vp%u: no output buffer (%s)", p->vp, strerror(errno));
			return -1;
		}
		kfd = dup(fd);	/* one for the kernel, one owned by Vulkan */
		if (kfd < 0 || import_dmabuf(fd, &p->out[i])) {
			LOGE("vp%u: cannot import an output buffer", p->vp);
			return -1;
		}
		st.fds[i] = kfd;
	}
	VK(vkAllocateDescriptorSets(dev, &dai, &p->set));
	VK(vkAllocateCommandBuffers(dev, &cai, &p->cmd));
	if (ioctl(odfd, GAMMAOS_OD_SETUP, &st)) {
		LOGE("vp%u: setup failed (%s)", p->vp, strerror(errno));
		return -1;
	}
	for (i = 0; i < GAMMAOS_OD_MAX_OUT; i++)
		close(st.fds[i]);
	LOGI("vp%u: %ux%u ready", p->vp, p->w, p->h);
	return 0;
}

static struct src_buf *src_find(struct port *p, uint64_t id)
{
	int i;

	for (i = 0; i < MAX_SRC; i++)
		if (p->src[i].id == id)
			return &p->src[i];
	return NULL;
}

static void src_forget(struct port *ports, int np, uint64_t id)
{
	int k, i;

	for (k = 0; k < np; k++)
		for (i = 0; i < MAX_SRC; i++)
			if (ports[k].src[i].id == id) {
				vbuf_free(&ports[k].src[i].b);
				ports[k].src[i].id = 0;
			}
}

static struct src_buf *src_add(struct port *p, uint64_t id, int fd, unsigned pitch_bytes)
{
	struct src_buf *s = src_find(p, 0);

	if (!s) {
		uint64_t oldest = UINT64_MAX;
		int i, o = 0;

		for (i = 0; i < MAX_SRC; i++)
			if (p->src[i].id < oldest) {
				oldest = p->src[i].id;
				o = i;
			}
		src_forget(p, 1, oldest);
		s = &p->src[o];
	}
	if (import_dmabuf(fd, &s->b)) {
		LOGE("vp%u: cannot import a source frame", p->vp);
		close(fd);
		return NULL;
	}
	s->id = id;
	s->pitch = pitch_bytes / 4;
	return s;
}

static int render(struct port *p, const struct gammaos_od_job *job)
{
	struct src_buf *s = src_find(p, job->src_id), *pv;
	VkDescriptorBufferInfo bi[4];
	VkWriteDescriptorSet w[4];
	VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
					 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &p->cmd };
	struct push pc;
	int i;

	if (!s && job->src_fd >= 0)
		s = src_add(p, job->src_id, job->src_fd, job->src_pitch);
	if (!s)
		return -ENOENT;
	pv = p->last_src ? src_find(p, p->last_src) : NULL;

	bi[0] = (VkDescriptorBufferInfo){ s->b.buf, 0, VK_WHOLE_SIZE };
	bi[1] = (VkDescriptorBufferInfo){ (pv ? pv : s)->b.buf, 0, VK_WHOLE_SIZE };
	bi[2] = (VkDescriptorBufferInfo){ p->state.buf, 0, VK_WHOLE_SIZE };
	bi[3] = (VkDescriptorBufferInfo){ p->out[job->out_index].buf, 0, VK_WHOLE_SIZE };
	for (i = 0; i < 4; i++)
		w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p->set,
					       .dstBinding = (uint32_t)i, .descriptorCount = 1,
					       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					       .pBufferInfo = &bi[i] };
	vkUpdateDescriptorSets(dev, 4, w, 0, NULL);

	pc = (struct push){ .w = p->w, .h = p->h, .src_pitch = s->pitch, .prev_pitch = (pv ? pv : s)->pitch,
			    .out_pitch = p->w, .held = pv ? (float)job->held_frames : 0.0f,
			    .init = p->primed ? 0.0f : 1.0f,
			    .p0 = { prof.rise_black / 100.0f, prof.rise_white / 100.0f, prof.curve / 16.0f,
				    prof.fall / 100.0f },
			    .p1 = { prof.thr, prof.cap_up, prof.cap_dn, 0.0f } };

	VK(vkResetCommandBuffer(p->cmd, 0));
	VK(vkBeginCommandBuffer(p->cmd, &cbi));
	vkCmdBindPipeline(p->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, od_pipe);
	vkCmdBindDescriptorSets(p->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &p->set, 0, NULL);
	vkCmdPushConstants(p->cmd, playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatch(p->cmd, (p->w + 15) / 16, (p->h + 15) / 16, 1);
	VK(vkEndCommandBuffer(p->cmd));
	VK(vkQueueSubmit(queue, 1, &si, fence));
	if (vkWaitForFences(dev, 1, &fence, VK_TRUE, 20000000ull) != VK_SUCCESS) {
		/* far too late for this refresh: let it finish so the fence and the
		 * command buffer are reusable, and report the frame as failed */
		vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
		vkResetFences(dev, 1, &fence);
		LOGE("vp%u: frame took longer than 20 ms", p->vp);
		return -1;
	}
	VK(vkResetFences(dev, 1, &fence));
	p->primed = 1;
	p->last_src = job->src_id;
	return 0;
}

int main(int argc, char **argv)
{
	struct port ports[2];
	time_t mt = 0;
	int np = 0, odfd, i;
	uint64_t frames = 0;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <profile> <vp>:<w>x<h> [<vp>:<w>x<h>]\n", argv[0]);
		return 2;
	}
	memset(ports, 0, sizeof(ports));
	for (i = 2; i < argc && np < 2; i++, np++)
		if (sscanf(argv[i], "%u:%ux%u", &ports[np].vp, &ports[np].w, &ports[np].h) != 3)
			return 2;
	{
		/* frames must make the same refresh: run ahead of ordinary work */
		struct sched_param sp = { .sched_priority = 2 };

		if (sched_setscheduler(0, SCHED_FIFO, &sp))
			setpriority(PRIO_PROCESS, 0, -10);
	}
	if (vk_init()) {
		LOGE("Vulkan setup failed");
		return 1;
	}
	odfd = open("/dev/gammaos_od", O_RDWR | O_CLOEXEC);
	if (odfd < 0) {
		LOGE("open /dev/gammaos_od: %s", strerror(errno));
		return 1;
	}
	load_profile(argv[1], &mt);
	for (i = 0; i < np; i++)
		if (port_setup(odfd, &ports[i]))
			return 1;

	for (;;) {
		struct gammaos_od_job job;
		struct gammaos_od_done done = { 0 };
		struct port *p = NULL;

		if (ioctl(odfd, GAMMAOS_OD_WAIT, &job)) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			LOGE("wait: %s", strerror(errno));
			return 1;
		}
		for (i = 0; i < GAMMAOS_OD_MAX_EVICT; i++)
			if (job.evict[i])
				src_forget(ports, np, job.evict[i]);
		for (i = 0; i < np; i++)
			if (ports[i].vp == job.vp)
				p = &ports[i];
		done.vp = job.vp;
		done.seq = job.seq;
		if (!p || job.out_index >= GAMMAOS_OD_MAX_OUT) {
			if (job.src_fd >= 0)
				close(job.src_fd);
			done.status = -EINVAL;
		} else {
			done.status = render(p, &job) ? -EIO : 0;
		}
		ioctl(odfd, GAMMAOS_OD_DONE, &done);
		if ((++frames & 63) == 0)
			load_profile(argv[1], &mt);
	}
}
