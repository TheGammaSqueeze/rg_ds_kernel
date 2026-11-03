// SPDX-License-Identifier: GPL-2.0
/*
 * odtest: overdrive prototype and motion test pattern for one RG DS Plus panel.
 *
 * Takes DRM master (the nano home must be stopped first), shows the four-box
 * motion pattern from /work/kpa/BFI_AND_OVERDRIVE.md section 7 at DS native size
 * (256x192), applies the KPA software overdrive to the native RGB565 frame, then
 * scales it 4x into a 1024x768 XRGB8888 scanout buffer and page flips every vblank.
 *
 * Knobs are reread from a file while running (one line, space separated):
 *   on kuphi kuplo gam kdn thr capup capdn head [period_frames] [mode] [kdn_g] [kdn_b] [rowk]
 *   default: 1 8 2 22 6 2 3 2 0 90 0   (the KPA shipping profile)
 *   kdn_g / kdn_b (mode 1 only): falling alpha for the green and blue channels; kdn is red.
 *   When absent they follow kdn.
 *   rowk (mode 1 only): row-aware response for backlight strobing. Lower rows are written
 *   closer to the strobe and have less time to settle, so their alpha falls linearly from
 *   the knob value at the top row to (100 - rowk) percent of it at the bottom row.
 *
 * mode 2: response calibration. Ten gray patches (5 x 2), each switching between a pair of
 *   the levels 0, 64, 128, 192, 255 every period/2 frames, all at the same frame, overdrive off.
 *   Filmed in slow motion it gives the panel's real response for 20 transitions.
 * mode 0: the KPA rule (overshoot from what the previous frame asked for).
 * mode 1: panel response model. Each pixel keeps an estimate of the level the liquid
 *   crystal has actually reached; each frame the crystal covers a fraction alpha of the
 *   remaining distance to what it is driven with. The drive is chosen so the estimate
 *   lands on the target this frame (clamped to the rails and the caps), then the estimate
 *   is advanced with what that drive really achieves. Unfinished transitions, thin lines
 *   and rail-limited edges are then handled from where the pixel really is.
 *   Knob meaning in mode 1: kuphi / kuplo = alpha rising from a black / white origin in
 *   percent, kdn = alpha falling in percent, gam = the same origin curve, thr = deadband,
 *   capup / capdn = maximum overshoot beyond the target (5-bit levels, doubled for green).
 *
 * Usage: odtest <connector: DSI-1|DSI-2> <seconds> [knob file] [pixel clock kHz]
 *   With a pixel clock, the panel is driven faster at the same refresh: the
 *   horizontal timing is kept and the extra lines go into the vertical front
 *   porch, so the rows are written in less of the frame and the frame ends with
 *   a long blank (for backlight strobing). The mode ends with the run.
 * Built static (no libdrm), so it runs on Android as is.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#define NW 256
#define NH 192
#define SCALE 4

struct knobs {
	int on, kuphi, kuplo, gam, kdn, thr, capup, capdn, head, period, mode, kdn_g, kdn_b, rowk;
};

static struct knobs K = { 1, 8, 2, 22, 6, 2, 3, 2, 0, 90, 0, 6, 6, 0 };
static float up5[32], up6[64];
/* mode 1: estimated reached level per pixel and channel, in channel units */
static float est[3][256 * 192];
static volatile sig_atomic_t stop;

static void on_sig(int s) { (void)s; stop = 1; }

static void rebuild_tables(void)
{
	for (int m = 0; m < 2; m++) {
		int max = m ? 63 : 31;
		float *t = m ? up6 : up5;

		for (int p = 0; p <= max; p++) {
			float frac = (float)(max - p) / max;
			float wf = frac + (frac * frac - frac) * (K.gam - 16) / 16.0f;

			t[p] = K.kuplo + (K.kuphi - K.kuplo) * wf;
		}
	}
}

static int load_knobs(const char *path, time_t *mt)
{
	struct stat st;
	struct knobs k = K;
	FILE *f;
	int n;

	if (!path || stat(path, &st) || st.st_mtime == *mt)
		return 0;
	*mt = st.st_mtime;
	f = fopen(path, "r");
	if (!f)
		return 0;
	k.mode = 0;
	k.kdn_g = -1;
	k.kdn_b = -1;
	k.rowk = 0;
	n = fscanf(f, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d", &k.on, &k.kuphi, &k.kuplo, &k.gam,
		   &k.kdn, &k.thr, &k.capup, &k.capdn, &k.head, &k.period, &k.mode, &k.kdn_g, &k.kdn_b,
		   &k.rowk);
	if (k.rowk < 0 || k.rowk > 90)
		k.rowk = 0;
	if (k.kdn_g < 0)
		k.kdn_g = k.kdn;
	if (k.kdn_b < 0)
		k.kdn_b = k.kdn;
	fclose(f);
	if (n < 9)
		return 0;
	if (k.period < 10)
		k.period = 90;
	K = k;
	rebuild_tables();
	printf("knobs: on=%d kuphi=%d kuplo=%d gam=%d kdn=%d thr=%d capup=%d capdn=%d head=%d period=%d mode=%d kdn_g=%d kdn_b=%d rowk=%d\n",
	       K.on, K.kuphi, K.kuplo, K.gam, K.kdn, K.thr, K.capup, K.capdn, K.head, K.period, K.mode,
	       K.kdn_g, K.kdn_b, K.rowk);
	fflush(stdout);
	return 1;
}

/* one channel of the KPA rule; mul is 1 for 5-bit channels, 2 for green */
static int od_ch(int c, int p, int max, const float *up, int mul)
{
	int head = K.head * mul;
	int d, ov;

	if (head) {
		c = head + c * (max - 2 * head) / max;
		p = head + p * (max - 2 * head) / max;
	}
	d = c - p;
	if (d <= K.thr && d >= -K.thr)
		return c;
	if (d > 0) {
		ov = (int)(up[p] * d / 16.0f + 0.5f);
		if (ov > K.capup * mul)
			ov = K.capup * mul;
	} else {
		ov = -(int)(K.kdn * -d / 16.0f + 0.5f);
		if (ov < -K.capdn * mul)
			ov = -K.capdn * mul;
	}
	c += ov;
	return c < 0 ? 0 : c > max ? max : c;
}

/*
 * mode 1, one channel: e is the estimated reached level (updated in place), c the target.
 * up[] holds the rising alpha (percent) by origin level, from the same origin curve.
 */
static int od_model(int c, float *e, int max, const float *up, int mul, int kdn, float rs)
{
	int head = K.head * mul;
	float a, drive, lo, hi;

	if (head)
		c = head + c * (max - 2 * head) / max;
	if (c - *e <= K.thr * mul && c - *e >= -K.thr * mul) {
		*e += (c - *e) * 0.5f;
		return c;
	}
	if (c > *e) {
		int o = (int)(*e + 0.5f);

		a = up[o < 0 ? 0 : o > max ? max : o] / 100.0f;
		hi = c + K.capup * mul;
		lo = c;
	} else {
		a = kdn / 100.0f;
		lo = c - K.capdn * mul;
		hi = c;
	}
	a *= rs;
	if (a < 0.05f)
		a = 0.05f;
	if (a > 1.0f)
		a = 1.0f;
	drive = *e + (c - *e) / a;
	if (drive > hi)
		drive = hi;
	if (drive < lo)
		drive = lo;
	if (drive > max)
		drive = max;
	if (drive < 0)
		drive = 0;
	drive = (int)(drive + 0.5f);
	*e += (drive - *e) * a;
	return (int)drive;
}

static uint16_t rgb(int r, int g, int b) { return (uint16_t)(r << 11 | g << 5 | b); }

/* gray field with four outlined boxes (red, green, blue, yellow) sweeping together */
static void draw_pattern(uint16_t *fb, unsigned frame)
{
	static const uint16_t col[4] = { 0xF800, 0x07E0, 0x001F, 0xFFE0 };
	const int bw = 40, bh = 34, gap = 12, top = (NH - 4 * bh - 3 * gap) / 2;
	unsigned ph = frame % (unsigned)K.period, half = K.period / 2;
	int span = NW - bw, x;

	x = ph < half ? (int)(ph * span / half) : (int)((K.period - ph) * span / (K.period - half));
	for (int i = 0; i < NW * NH; i++)
		fb[i] = rgb(16, 32, 16);
	for (int b = 0; b < 4; b++) {
		int y0 = top + b * (bh + gap);

		for (int y = y0; y < y0 + bh; y++)
			for (int xx = x; xx < x + bw; xx++) {
				int edge = y == y0 || y == y0 + bh - 1 || xx == x || xx == x + bw - 1;

				fb[y * NW + xx] = edge ? 0 : col[b];
			}
	}
}

static void draw_cal(uint16_t *fb, unsigned frame)
{
	static const int lv[5] = { 0, 64, 128, 192, 255 };
	static const int pa[10][2] = { {0,1},{0,2},{0,3},{0,4},{1,2},{1,3},{1,4},{2,3},{2,4},{3,4} };
	int high = (frame / (unsigned)(K.period / 2)) & 1;
	const int pw = NW / 5, ph = NH / 2, m = 3;

	for (int i = 0; i < NW * NH; i++)
		fb[i] = 0;
	for (int p = 0; p < 10; p++) {
		int v = lv[pa[p][high]];
		uint16_t c = rgb(v >> 3, v >> 2, v >> 3);
		int x0 = (p % 5) * pw + m, y0 = (p / 5) * ph + m;

		for (int y = y0; y < y0 + ph - 2 * m; y++)
			for (int x = x0; x < x0 + pw - 2 * m; x++)
				fb[y * NW + x] = c;
	}
}

static void overdrive(const uint16_t *cur, uint16_t *prev, uint16_t *out, int have_prev)
{
	for (int i = 0; i < NW * NH; i++) {
		uint16_t c = cur[i], p = prev[i];

		if (K.on && K.mode == 1) {
			float rs = 1.0f - (float)K.rowk / 100.0f * (float)(i / NW) / (NH - 1);

			if (!have_prev) {
				est[0][i] = c >> 11;
				est[1][i] = (c >> 5) & 63;
				est[2][i] = c & 31;
				out[i] = c;
			} else {
				int r = od_model(c >> 11, &est[0][i], 31, up5, 1, K.kdn, rs);
				int g = od_model((c >> 5) & 63, &est[1][i], 63, up6, 2, K.kdn_g, rs);
				int b = od_model(c & 31, &est[2][i], 31, up5, 1, K.kdn_b, rs);

				out[i] = rgb(r, g, b);
			}
		} else if (!K.on || !have_prev) {
			out[i] = c;
		} else {
			int r = od_ch(c >> 11, p >> 11, 31, up5, 1);
			int g = od_ch((c >> 5) & 63, (p >> 5) & 63, 63, up6, 2);
			int b = od_ch(c & 31, p & 31, 31, up5, 1);

			out[i] = rgb(r, g, b);
		}
		prev[i] = c;	/* history is the un-overdriven source, as on the KPA */
	}
}

static void scale_out(const uint16_t *src, uint32_t *dst, int pitch_px)
{
	uint32_t row[NW * SCALE];

	for (int y = 0; y < NH; y++) {
		for (int x = 0; x < NW; x++) {
			uint16_t v = src[y * NW + x];
			uint32_t r = v >> 11, g = (v >> 5) & 63, b = v & 31;
			uint32_t px = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);

			for (int s = 0; s < SCALE; s++)
				row[x * SCALE + s] = px;
		}
		for (int s = 0; s < SCALE; s++)
			memcpy(dst + (size_t)(y * SCALE + s) * pitch_px, row, sizeof(row));
	}
}

struct buf {
	uint32_t handle, fb_id, pitch;
	uint64_t size;
	uint32_t *map;
};

static int make_buf(int fd, int w, int h, struct buf *b)
{
	struct drm_mode_create_dumb cd = { .width = w, .height = h, .bpp = 32 };
	struct drm_mode_fb_cmd fc = { 0 };
	struct drm_mode_map_dumb md = { 0 };

	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd))
		return -1;
	b->handle = cd.handle;
	b->pitch = cd.pitch;
	b->size = cd.size;
	fc.width = w;
	fc.height = h;
	fc.pitch = cd.pitch;
	fc.bpp = 32;
	fc.depth = 24;
	fc.handle = cd.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fc))
		return -1;
	b->fb_id = fc.fb_id;
	md.handle = cd.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
		return -1;
	b->map = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	return b->map == MAP_FAILED ? -1 : 0;
}

static const char *ctype(uint32_t t)
{
	return t == DRM_MODE_CONNECTOR_DSI ? "DSI" : t == DRM_MODE_CONNECTOR_WRITEBACK ? "Writeback" : "Other";
}

int main(int argc, char **argv)
{
	struct drm_mode_card_res res = { 0 };
	uint32_t conns[16], crtcs[16], encs[16];
	struct drm_mode_get_connector gc;
	struct drm_mode_get_encoder ge = { 0 };
	struct drm_mode_crtc cr = { 0 };
	struct drm_mode_modeinfo modes[32];
	uint32_t conn_id = 0, crtc_id = 0;
	struct buf bufs[2];
	static uint16_t cur[NW * NH], prev[NW * NH], out[NW * NH];
	time_t mt = 0;
	unsigned frame = 0;
	int fd, secs, cur_buf = 0, have_prev = 0;
	struct timespec t0, t1;
	double work_ms = 0;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <DSI-1|DSI-2> <seconds> [knob file]\n", argv[0]);
		return 2;
	}
	secs = atoi(argv[2]);
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	rebuild_tables();
	load_knobs(argc > 3 ? argv[3] : NULL, &mt);

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0 || ioctl(fd, DRM_IOCTL_SET_MASTER, 0)) {
		perror("open/master (is the nano home stopped?)");
		return 1;
	}
	res.connector_id_ptr = (uintptr_t)conns;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.encoder_id_ptr = (uintptr_t)encs;
	res.count_connectors = 16;
	res.count_crtcs = 16;
	res.count_encoders = 16;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
		perror("getresources");
		return 1;
	}
	for (uint32_t i = 0; i < res.count_connectors && !conn_id; i++) {
		char name[32];

		memset(&gc, 0, sizeof(gc));
		gc.connector_id = conns[i];
		gc.count_modes = 32;
		gc.modes_ptr = (uintptr_t)modes;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc))
			continue;
		snprintf(name, sizeof(name), "%s-%u", ctype(gc.connector_type), gc.connector_type_id);
		if (!strcmp(name, argv[1]) && gc.encoder_id) {
			conn_id = gc.connector_id;
			ge.encoder_id = gc.encoder_id;
		}
	}
	if (!conn_id || ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &ge) || !ge.crtc_id) {
		fprintf(stderr, "connector %s not found or not active\n", argv[1]);
		return 1;
	}
	crtc_id = ge.crtc_id;
	cr.crtc_id = crtc_id;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &cr) || !cr.mode_valid) {
		fprintf(stderr, "crtc %u has no mode\n", crtc_id);
		return 1;
	}
	printf("%s: connector %u crtc %u mode %ux%u@%u\n", argv[1], conn_id, crtc_id,
	       cr.mode.hdisplay, cr.mode.vdisplay, cr.mode.vrefresh);
	if (argc > 4 && atoi(argv[4]) > 0) {
		struct drm_mode_modeinfo m = cr.mode;
		uint32_t khz = (uint32_t)atoi(argv[4]);
		double hz = (double)m.clock * 1000.0 / ((double)m.htotal * m.vtotal);
		uint32_t vt = (uint32_t)((double)khz * 1000.0 / ((double)m.htotal * hz) + 0.5);
		uint32_t tail = m.vtotal - m.vsync_start;	/* sync + back porch */
		uint32_t slen = m.vsync_end - m.vsync_start;

		if (vt < m.vtotal || vt - tail <= m.vdisplay) {
			fprintf(stderr, "pixel clock %u kHz is below the current mode\n", khz);
			return 1;
		}
		m.clock = khz;
		m.vtotal = vt;
		m.vsync_start = vt - tail;
		m.vsync_end = m.vsync_start + slen;
		snprintf(m.name, sizeof(m.name), "%ux%u-fast", m.hdisplay, m.vdisplay);
		printf("fast scan: clock %u -> %u kHz, vtotal %u -> %u (vfp %u), refresh %.3f Hz, rows written in %.2f of %.2f ms\n",
		       cr.mode.clock, m.clock, cr.mode.vtotal, m.vtotal, m.vsync_start - m.vdisplay,
		       (double)m.clock * 1000.0 / ((double)m.htotal * m.vtotal),
		       (double)(m.vdisplay + tail) * m.htotal / m.clock,
		       (double)m.vtotal * m.htotal / m.clock);
		cr.mode = m;
	}
	if (cr.mode.hdisplay != NW * SCALE || cr.mode.vdisplay != NH * SCALE) {
		fprintf(stderr, "expected a %dx%d mode\n", NW * SCALE, NH * SCALE);
		return 1;
	}
	for (int i = 0; i < 2; i++)
		if (make_buf(fd, NW * SCALE, NH * SCALE, &bufs[i])) {
			perror("dumb buffer");
			return 1;
		}
	draw_pattern(cur, 0);
	scale_out(cur, bufs[0].map, bufs[0].pitch / 4);
	cr.fb_id = bufs[0].fb_id;
	cr.set_connectors_ptr = (uintptr_t)&conn_id;
	cr.count_connectors = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &cr)) {
		perror("setcrtc");
		return 1;
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (!stop) {
		struct drm_mode_crtc_page_flip pf = { 0 };
		struct timespec a, b;
		char ev[256];

		clock_gettime(CLOCK_MONOTONIC, &t1);
		if (t1.tv_sec - t0.tv_sec >= secs)
			break;
		if (frame % 30 == 0 && load_knobs(argc > 3 ? argv[3] : NULL, &mt))
			have_prev = 0;
		cur_buf ^= 1;
		clock_gettime(CLOCK_MONOTONIC, &a);
		if (K.mode == 2) {
			draw_cal(cur, frame);
			memcpy(out, cur, sizeof(out));
		} else {
			draw_pattern(cur, frame);
			overdrive(cur, prev, out, have_prev);
		}
		have_prev = 1;
		scale_out(out, bufs[cur_buf].map, bufs[cur_buf].pitch / 4);
		clock_gettime(CLOCK_MONOTONIC, &b);
		work_ms += (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
		pf.crtc_id = crtc_id;
		pf.fb_id = bufs[cur_buf].fb_id;
		pf.flags = DRM_MODE_PAGE_FLIP_EVENT;
		if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf)) {
			perror("page flip");
			break;
		}
		if (read(fd, ev, sizeof(ev)) <= 0)
			break;
		if (++frame % 300 == 0) {
			double el = (b.tv_sec - t0.tv_sec) + (b.tv_nsec - t0.tv_nsec) / 1e9;

			printf("frames=%u fps=%.2f work=%.2f ms/frame od=%d\n", frame, frame / el,
			       work_ms / frame, K.on);
			fflush(stdout);
		}
	}
	printf("done: %u frames\n", frame);
	ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
	close(fd);
	return 0;
}
