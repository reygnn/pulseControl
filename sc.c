/*
 * sc - a minimal volume control for PulseAudio on Wayland, in the Motif look
 * of mwc (https://github.com/reygnn/mwc) and mfm.
 *
 * libpulse, libwayland, Pango and Cairo, one file, no toolkit. It controls the
 * default output and follows it when it changes, or when something else
 * changes its volume. It reads the same colours as mwc: MWC_INACTIVE /
 * MWC_ACTIVE from the environment, else Mwm*background and Mwm*activeBackground
 * from ~/.config/mwc/mwmrc, else the Motif defaults.
 *
 * Keys:   Up / Right / +  louder        Down / Left / -  quieter
 *         m / Space       mute          Esc / q / Ctrl+Q / Ctrl+W  quit
 *         also the volume keys of the keyboard, while the window has the focus
 * Mouse:  click or drag the scale, wheel anywhere, click the speaker to mute.
 *
 * The scale ends at 100 %. A volume above that (set by another program) is
 * shown and can be lowered, but sc does not raise anything past 100 %.
 *
 * Everything runs in one thread: Pulse's main loop does the waiting, and its
 * poll function watches the Wayland socket as well.
 */
#define _GNU_SOURCE
#include <cairo.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <pulse/pulseaudio.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define BEV 2           /* bevel width, as in mwm */
#define PAD 6
#define THUMB 30        /* width of the scale's slider */
#define STEP 5          /* percent per key press or wheel step */
#define RETRY_MS 1000   /* between attempts to reach the sound server */
#ifndef MAX            /* glib has them too */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

struct rect { int x, y, w, h; };

/* ---- state -------------------------------------------------------------- */

static pa_mainloop *ml;
static pa_context *ctx;
static int ctx_failed;          /* it died: replace it from the main loop */
static uint64_t retry_at;

enum { SINK_UNKNOWN, SINK_NONE, SINK_OK };
static int sink_state = SINK_UNKNOWN;
static uint32_t sink_idx;
static int sink_mute, sink_pct;
static char sink_desc[256];
/* The sink's volume with its balance. A volume is always scaled from this, as
 * zero would lose the balance when dragging down to it and back. */
static pa_cvolume sink_shape;
/* Changes of ours on their way. What the server reports until the last one is
 * acknowledged, or during a drag, is older than what the window shows, and
 * would make the slider jump back. */
static int pending;

static int dragging, drag_off, btn_pressed;
static int dirty = 1;

/* ---- Wayland ------------------------------------------------------------ */

/* One wl_shm buffer with a Cairo context on it. */
struct shmbuf {
	struct wl_buffer *wl;
	cairo_surface_t *cs;
	cairo_t *cr;
	void *data;
	size_t size;
	int w, h;
	int busy;       /* the compositor still reads it */
	int stale;      /* wrong size: free it as soon as it is released */
};

static struct wl_display *dpy;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct zxdg_decoration_manager_v1 *deco_mgr;
static struct wl_surface *surface;
static struct wl_callback *frame;       /* a frame is on its way */
static struct shmbuf *bufs[2];
static struct xdg_surface *main_xdg;
static struct xdg_toplevel *toplevel;
static int configured;          /* the first configure has arrived */
static int need_commit;         /* a configure waits for its commit */
static int pend_w, pend_h;      /* size of the configure in progress */
static int pend_bound;          /* ... which is maximized, tiled or the like */

static struct wl_pointer *pointer;
static struct wl_surface *ptr_surface;
static int ptr_x, ptr_y;
static double axis_acc;
static int axis_steps;
static struct wl_surface *cursor_surf;
static struct wl_cursor_image *cursor_img;

static struct wl_keyboard *keyboard;
static struct xkb_context *xkb;
static struct xkb_keymap *kb_map;
static struct xkb_state *kb_state;
static uint32_t repeat_kc;      /* xkb keycode of the repeating key, or 0 */
static uint64_t repeat_next;
static int repeat_rate = 25, repeat_delay = 600;

static int W = 320, H, float_w = 320, min_h;
static uint32_t px_bg, px_top, px_bot, px_trough, px_sel;
static uint32_t xc_fg, xc_dim, xc_err;
static cairo_t *cr;             /* what fill, bevel and text draw on */
static PangoLayout *plo;
static int font_asc, font_desc;
static int barh;
static struct rect r_btn, r_scale, r_pct, r_status;

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- buffers ------------------------------------------------------------ */

static void buf_free(struct shmbuf *b)
{
	cairo_destroy(b->cr);
	cairo_surface_destroy(b->cs);
	wl_buffer_destroy(b->wl);
	munmap(b->data, b->size);
	free(b);
}

static void buf_release(void *data, struct wl_buffer *wl)
{
	struct shmbuf *b = data;
	b->busy = 0;
	if (b->stale)
		buf_free(b);
}

static const struct wl_buffer_listener buf_listener = { buf_release };

static struct shmbuf *buf_new(int w, int h)
{
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, w);
	size_t size = (size_t)stride * h;
	struct shmbuf *b = calloc(1, sizeof *b);
	int fd = memfd_create("sc", MFD_CLOEXEC);
	void *data = MAP_FAILED;
	if (b && fd >= 0 && ftruncate(fd, size) == 0)
		data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "sc: no memory for a %dx%d buffer: %s\n", w, h,
			strerror(errno));
		exit(1);
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(b->wl, &buf_listener, b);
	b->data = data;
	b->size = size;
	b->w = w;
	b->h = h;
	b->cs = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_RGB24, w, h,
		stride);
	b->cr = cairo_create(b->cs);
	cairo_set_antialias(b->cr, CAIRO_ANTIALIAS_NONE);       /* not the text */
	return b;
}

/* A buffer of this size to draw on, or NULL while the compositor holds both;
 * its release wakes the main loop. */
static struct shmbuf *get_buffer(int w, int h)
{
	for (int i = 0; i < 2; i++) {
		struct shmbuf *b = bufs[i];
		if (!b || (b->w == w && b->h == h))
			continue;
		if (b->busy)
			b->stale = 1;
		else
			buf_free(b);
		bufs[i] = NULL;
	}
	for (int i = 0; i < 2; i++)
		if (bufs[i] && !bufs[i]->busy)
			return bufs[i];
	for (int i = 0; i < 2; i++)
		if (!bufs[i])
			return bufs[i] = buf_new(w, h);
	return NULL;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	wl_callback_destroy(cb);
	frame = NULL;
}

static const struct wl_callback_listener frame_listener = { frame_done };

/* ---- colours (same rules as mwm's draw.c) ------------------------------- */

static uint32_t mix(uint32_t rgb, uint32_t to, float f)
{
	uint32_t out = 0;
	for (int sh = 0; sh <= 16; sh += 8) {
		float c = (rgb >> sh) & 0xff, t = (to >> sh) & 0xff;
		out |= (uint32_t)(c + (t - c) * f + 0.5f) << sh;
	}
	return out;
}

static uint32_t contrast(uint32_t bg)
{
	unsigned r = (bg >> 16) & 0xff, g = (bg >> 8) & 0xff, b = bg & 0xff;
	return 299 * r + 587 * g + 114 * b > 140000 ? 0x000000 : 0xffffff;
}

/* What mwc reads: "#rrggbb", names like "gold", and rgb:r/g/b with 1 to 4 hex
 * digits per channel. */
static int parse_color(const char *s, uint32_t *out)
{
	PangoColor c;
	if (!s || !*s)
		return 0;
	if (!strncasecmp(s, "rgb:", 4)) {
		uint32_t rgb = 0;
		s += 4;
		for (int i = 0; i < 3; i++) {
			char *end;
			unsigned long v = strtoul(s, &end, 16);
			int digits = end - s;
			if (digits < 1 || digits > 4 || *end != (i < 2 ? '/' : '\0'))
				return 0;
			unsigned long max = (1ul << (4 * digits)) - 1;
			rgb = rgb << 8 | (v * 255 + max / 2) / max;
			s = end + 1;
		}
		*out = rgb;
		return 1;
	}
	if (!pango_color_parse(&c, s))
		return 0;
	*out = (uint32_t)(c.red >> 8) << 16 | (c.green >> 8) << 8 | (c.blue >> 8);
	return 1;
}

/* Last global "Mwm*key: value" line of ~/.config/mwc/mwmrc. */
static char *mwmrc_get(const char *key, char *val, size_t n)
{
	char path[PATH_MAX], line[512];
	const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
	int len;
	if (xdg && *xdg)
		len = snprintf(path, sizeof path, "%s/mwc/mwmrc", xdg);
	else
		len = snprintf(path, sizeof path, "%s/.config/mwc/mwmrc",
			home ? home : "");
	if (len < 0 || (size_t)len >= sizeof path)
		return NULL;
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char *found = NULL;
	size_t kl = strlen(key);
	while (fgets(line, sizeof line, f)) {
		char *p = line;
		while (isspace((unsigned char)*p))
			p++;
		/* As mwc reads it: any case, "." or "*", blanks before the ":". */
		char *colon = strchr(p, ':');
		if (!colon)
			continue;
		char *ke = colon;
		while (ke > p && isspace((unsigned char)ke[-1]))
			ke--;
		if (strncasecmp(p, "mwm", 3) || (p[3] != '*' && p[3] != '.') ||
		    (size_t)(ke - (p + 4)) != kl || strncasecmp(p + 4, key, kl))
			continue;
		p = colon + 1;
		while (isspace((unsigned char)*p))
			p++;
		char *e = p + strlen(p);
		while (e > p && isspace((unsigned char)e[-1]))
			*--e = 0;
		if ((size_t)snprintf(val, n, "%s", p) < n)
			found = val;
	}
	fclose(f);
	return found;
}

static uint32_t setting(const char *env, const char *res, uint32_t def)
{
	uint32_t rgb;
	char v[256];
	if (parse_color(getenv(env), &rgb))
		return rgb;
	if (parse_color(mwmrc_get(res, v, sizeof v), &rgb))
		return rgb;
	return def;
}

static void theme_init(void)
{
	uint32_t bg = setting("MWC_INACTIVE", "background", 0xaeb2c3);
	uint32_t fg = contrast(bg);

	px_bg = bg;
	px_top = mix(bg, 0xffffff, 0.55f);
	px_bot = mix(bg, 0x000000, 0.5f);
	px_trough = mix(bg, 0x000000, 0.12f);
	px_sel = setting("MWC_ACTIVE", "activeBackground", 0x7387ad);
	xc_fg = fg;
	xc_dim = mix(bg, fg, 0.6f);
	xc_err = 0x8a0303;

	const char *f = getenv("SC_FONT");
	PangoFontDescription *desc =
		pango_font_description_from_string(f && *f ? f : "Sans 10");
	/* "Monospace" has no size, and metrics without one are all zero. */
	if (pango_font_description_get_size(desc) <= 0)
		pango_font_description_set_size(desc, 10 * PANGO_SCALE);
	PangoContext *pctx = pango_font_map_create_context(
		pango_cairo_font_map_get_default());
	plo = pango_layout_new(pctx);
	pango_layout_set_font_description(plo, desc);
	pango_layout_set_single_paragraph_mode(plo, TRUE);
	PangoFontMetrics *m = pango_context_get_metrics(pctx, desc, NULL);
	font_asc = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(m));
	font_desc = PANGO_PIXELS_CEIL(pango_font_metrics_get_descent(m));
	pango_font_metrics_unref(m);
	pango_font_description_free(desc);
	barh = font_asc + font_desc + 10;
	min_h = H = 2 * PAD + 2 * barh + 4;
}

/* ---- PulseAudio --------------------------------------------------------- */

static void sink_info(pa_context *c, const pa_sink_info *i, int eol, void *u)
{
	if (eol > 0 || pending || dragging)
		return;
	dirty = 1;
	if (eol < 0 || !i) {
		sink_state = SINK_NONE;
		return;
	}
	pa_volume_t vol = pa_cvolume_max(&i->volume);
	if (vol != PA_VOLUME_MUTED || sink_state != SINK_OK ||
	    i->index != sink_idx)
		sink_shape = i->volume;
	sink_idx = i->index;
	sink_mute = i->mute;
	sink_pct = ((uint64_t)vol * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM;
	sink_state = SINK_OK;
	char *d = pa_utf8_filter(i->description ? i->description : i->name);
	snprintf(sink_desc, sizeof sink_desc, "%s", d ? d : "");
	pa_xfree(d);
}

/* Ask for the default sink, whichever that is by now. */
static void refresh(void)
{
	if (!ctx || pa_context_get_state(ctx) != PA_CONTEXT_READY)
		return;
	pa_operation *op = pa_context_get_sink_info_by_name(ctx, "@DEFAULT_SINK@",
		sink_info, NULL);
	if (op)
		pa_operation_unref(op);
}

static void set_done(pa_context *c, int success, void *u)
{
	if (pending > 0)
		pending--;
	if (!pending && !dragging)
		refresh();
}

/* Keep count of an operation that reports to set_done. */
static void track(pa_operation *op)
{
	if (!op)
		return;
	pending++;
	pa_operation_unref(op);
}

static void set_percent(int pct)
{
	if (sink_state != SINK_OK || pct == sink_pct)
		return;
	sink_pct = pct;
	dirty = 1;
	pa_cvolume cv = sink_shape;
	pa_cvolume_scale(&cv, ((uint64_t)pct * PA_VOLUME_NORM + 50) / 100);
	track(pa_context_set_sink_volume_by_index(ctx, sink_idx, &cv, set_done,
		NULL));
}

static void step_volume(int d)
{
	if (d > 0)
		set_percent(MIN(sink_pct + d, MAX(100, sink_pct)));
	else
		set_percent(MAX(sink_pct + d, 0));
}

static void toggle_mute(void)
{
	if (sink_state != SINK_OK)
		return;
	sink_mute = !sink_mute;
	dirty = 1;
	track(pa_context_set_sink_mute_by_index(ctx, sink_idx, sink_mute, set_done,
		NULL));
}

static void ctx_event(pa_context *c, pa_subscription_event_type_t t, uint32_t idx,
	void *u)
{
	/* Otherwise the last acknowledgement or the release asks. */
	if (!pending && !dragging)
		refresh();
}

static void ctx_state(pa_context *c, void *u)
{
	switch (pa_context_get_state(c)) {
	case PA_CONTEXT_READY: {
		pa_context_set_subscribe_callback(c, ctx_event, NULL);
		pa_operation *op = pa_context_subscribe(c,
			PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER,
			NULL, NULL);
		if (op)
			pa_operation_unref(op);
		refresh();
		break;
	}
	case PA_CONTEXT_FAILED:
	case PA_CONTEXT_TERMINATED:
		ctx_failed = 1;
		break;
	default:
		break;
	}
}

static void ctx_start(void)
{
	ctx = pa_context_new(pa_mainloop_get_api(ml), "sc");
	if (!ctx) {
		fprintf(stderr, "sc: cannot set up a PulseAudio context\n");
		exit(1);
	}
	pa_context_set_state_callback(ctx, ctx_state, NULL);
	/* It is a server's job to run; a volume control starts none. */
	if (pa_context_connect(ctx, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0)
		ctx_failed = 1;
	dirty = 1;
}

/* The context can't be used again after a failure, and a callback must not
 * free the one calling it, so this runs from the main loop. */
static void ctx_drop(void)
{
	pa_context_set_state_callback(ctx, NULL, NULL);
	pa_context_set_subscribe_callback(ctx, NULL, NULL);
	pa_context_disconnect(ctx);
	pa_context_unref(ctx);
	ctx = NULL;
	ctx_failed = 0;
	pending = 0;
	dragging = 0;
	sink_state = SINK_UNKNOWN;
	retry_at = now_ms() + RETRY_MS;
	dirty = 1;
}

/* ---- drawing ------------------------------------------------------------ */

static void set_rgb(uint32_t c)
{
	cairo_set_source_rgb(cr, ((c >> 16) & 0xff) / 255.0,
		((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0);
}

static void fill(uint32_t px, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	set_rgb(px);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

static void bevel(struct rect r, int raised)
{
	uint32_t t = raised ? px_top : px_bot;
	uint32_t b = raised ? px_bot : px_top;
	for (int i = 0; i < BEV; i++) {
		fill(t, r.x + i, r.y + i, r.w - 2 * i, 1);
		fill(t, r.x + i, r.y + i, 1, r.h - 2 * i);
		fill(b, r.x + i + 1, r.y + r.h - 1 - i, r.w - 2 * i - 1, 1);
		fill(b, r.x + r.w - 1 - i, r.y + i + 1, 1, r.h - 2 * i - 1);
	}
}

static int textw(const char *s)
{
	int w;
	pango_layout_set_text(plo, s, -1);
	pango_layout_get_pixel_size(plo, &w, NULL);
	return w;
}

/* s in at most maxw pixels (with "..." at the end if it is longer). */
static void text(uint32_t c, int x, int y, int h, int maxw, const char *s)
{
	int base = y + (h - (font_asc + font_desc)) / 2 + font_asc;
	pango_layout_set_text(plo, s, -1);
	pango_layout_set_width(plo, maxw * PANGO_SCALE);
	pango_layout_set_ellipsize(plo, PANGO_ELLIPSIZE_END);
	set_rgb(c);
	cairo_move_to(cr, x, base - pango_layout_get_baseline(plo) / PANGO_SCALE);
	pango_cairo_show_layout(cr, plo);
	pango_layout_set_width(plo, -1);
	pango_layout_set_ellipsize(plo, PANGO_ELLIPSIZE_NONE);
}

static void layout(void)
{
	char s[32];
	int pw = textw("100 %");
	/* Only something unusual is wider, such as a volume of 1500 %. */
	if (sink_state == SINK_OK) {
		snprintf(s, sizeof s, "%d %%", sink_pct);
		pw = MAX(pw, textw(s));
	}
	r_btn = (struct rect){ PAD, PAD, barh, barh };
	r_pct = (struct rect){ W - PAD - pw, PAD, pw, barh };
	r_scale = (struct rect){ PAD + barh + 4, PAD,
		W - 2 * PAD - barh - pw - 12, barh };
	r_status = (struct rect){ PAD, H - PAD - barh, W - 2 * PAD, barh };
}

static int scale_travel(void)
{
	return MAX(r_scale.w - 2 * BEV - THUMB, 1);
}

static int thumb_x(void)
{
	int p = sink_state == SINK_OK ? MIN(sink_pct, 100) : 0;
	return r_scale.x + BEV + (scale_travel() * p + 50) / 100;
}

/* A speaker, with a cross when muted and sound waves otherwise. */
static void draw_speaker(struct rect r, int muted, int off)
{
	int cy = r.y + r.h / 2 + off, a = r.h / 5;
	int x = r.x + r.w / 2 - 2 * a + off;
	set_rgb(sink_state == SINK_OK ? xc_fg : xc_dim);
	cairo_move_to(cr, x, cy - a / 2);
	cairo_line_to(cr, x + a, cy - a / 2);
	cairo_line_to(cr, x + 2 * a, cy - a - 1);
	cairo_line_to(cr, x + 2 * a, cy + a + 2);
	cairo_line_to(cr, x + a, cy + a / 2 + 1);
	cairo_line_to(cr, x, cy + a / 2 + 1);
	cairo_close_path(cr);
	cairo_fill(cr);
	int wx = x + 2 * a + 2;
	if (muted) {
		set_rgb(xc_err);
		cairo_set_line_width(cr, 2);
		cairo_move_to(cr, wx, cy - a + 1);
		cairo_line_to(cr, wx + a + 1, cy + a);
		cairo_move_to(cr, wx + a + 1, cy - a + 1);
		cairo_line_to(cr, wx, cy + a);
		cairo_stroke(cr);
	} else {
		fill(sink_state == SINK_OK ? xc_fg : xc_dim, wx, cy - a / 2, 1,
			a + 1);
		fill(sink_state == SINK_OK ? xc_fg : xc_dim, wx + 2, cy - a, 1,
			2 * a + 1);
	}
}

static void paint(void)
{
	char s[32];
	int ok = sink_state == SINK_OK;
	layout();
	fill(px_bg, 0, 0, W, H);

	/* mute button: stays in while muted */
	int down = (ok && sink_mute) != btn_pressed;
	bevel(r_btn, !down);
	draw_speaker(r_btn, ok && sink_mute, down);

	/* scale */
	int tx = thumb_x(), ix = r_scale.x + BEV, iy = r_scale.y + BEV;
	int iw = r_scale.w - 2 * BEV, ih = r_scale.h - 2 * BEV;
	fill(px_trough, ix, iy, iw, ih);
	if (ok && !sink_mute)
		fill(px_sel, ix, iy, tx - ix, ih);
	bevel(r_scale, 0);
	if (ok) {
		struct rect sl = { tx, iy, THUMB, ih };
		fill(px_bg, sl.x, sl.y, sl.w, sl.h);
		bevel(sl, 1);
		fill(px_bot, tx + THUMB / 2 - 1, iy + BEV, 1, ih - 2 * BEV);
		fill(px_top, tx + THUMB / 2, iy + BEV, 1, ih - 2 * BEV);

		snprintf(s, sizeof s, "%d %%", sink_pct);
		text(sink_mute ? xc_dim : xc_fg, r_pct.x + r_pct.w - textw(s),
			r_pct.y, r_pct.h, r_pct.w, s);
	}

	/* status line */
	bevel(r_status, 0);
	int sx = r_status.x + BEV + 4, sw = r_status.w - 2 * BEV - 8;
	if (ok)
		text(xc_fg, sx, r_status.y, r_status.h, sw, sink_desc);
	else if (sink_state == SINK_NONE)
		text(xc_err, sx, r_status.y, r_status.h, sw, "no output device");
	else if (!ctx || pa_context_get_state(ctx) != PA_CONTEXT_READY)
		text(retry_at ? xc_err : xc_dim, sx, r_status.y, r_status.h, sw,
			retry_at ? "no sound server" : "connecting ...");
}

/* 0 if the compositor holds both buffers: try again after a release. */
static int draw(void)
{
	struct shmbuf *b = get_buffer(W, H);
	if (!b)
		return 0;
	cr = b->cr;
	paint();
	cairo_surface_flush(b->cs);
	if (frame)
		wl_callback_destroy(frame);
	frame = wl_surface_frame(surface);
	wl_callback_add_listener(frame, &frame_listener, NULL);
	wl_surface_attach(surface, b->wl, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, b->w, b->h);
	wl_surface_commit(surface);
	b->busy = 1;
	return 1;
}

/* ---- input -------------------------------------------------------------- */

static int in(struct rect r, int x, int y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void scale_to(int x)
{
	int p = ((x - drag_off - r_scale.x - BEV) * 100 + scale_travel() / 2) /
		scale_travel();
	set_percent(MAX(MIN(p, 100), 0));
}

static void end_drag(void)
{
	if (!dragging)
		return;
	dragging = 0;
	if (!pending)
		refresh();
}

static void key_action(xkb_keysym_t ks)
{
	switch (ks) {
	case XKB_KEY_Up: case XKB_KEY_KP_Up:
	case XKB_KEY_Right: case XKB_KEY_KP_Right:
	case XKB_KEY_plus: case XKB_KEY_KP_Add:
	case XKB_KEY_XF86AudioRaiseVolume:
		step_volume(STEP);
		break;
	case XKB_KEY_Down: case XKB_KEY_KP_Down:
	case XKB_KEY_Left: case XKB_KEY_KP_Left:
	case XKB_KEY_minus: case XKB_KEY_KP_Subtract:
	case XKB_KEY_XF86AudioLowerVolume:
		step_volume(-STEP);
		break;
	case XKB_KEY_m: case XKB_KEY_M: case XKB_KEY_space:
	case XKB_KEY_XF86AudioMute:
		toggle_mute();
		break;
	case XKB_KEY_q: case XKB_KEY_Q: case XKB_KEY_Escape:
		exit(0);
	case XKB_KEY_w: case XKB_KEY_W:
		if (xkb_state_mod_name_is_active(kb_state, XKB_MOD_NAME_CTRL,
		    XKB_STATE_MODS_EFFECTIVE) > 0)
			exit(0);
		break;
	}
}

/* Only the volume repeats when a key is held. */
static int key_repeats(uint32_t kc)
{
	switch (xkb_state_key_get_one_sym(kb_state, kc)) {
	case XKB_KEY_Up: case XKB_KEY_KP_Up:
	case XKB_KEY_Right: case XKB_KEY_KP_Right:
	case XKB_KEY_plus: case XKB_KEY_KP_Add:
	case XKB_KEY_XF86AudioRaiseVolume:
	case XKB_KEY_Down: case XKB_KEY_KP_Down:
	case XKB_KEY_Left: case XKB_KEY_KP_Left:
	case XKB_KEY_minus: case XKB_KEY_KP_Subtract:
	case XKB_KEY_XF86AudioLowerVolume:
		return 1;
	}
	return 0;
}

static void kb_keymap(void *data, struct wl_keyboard *k, uint32_t format,
	int32_t fd, uint32_t size)
{
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *s = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (s == MAP_FAILED)
		return;
	struct xkb_keymap *map = xkb_keymap_new_from_string(xkb, s,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(s, size);
	struct xkb_state *state = map ? xkb_state_new(map) : NULL;
	if (!state) {
		xkb_keymap_unref(map);
		return;
	}
	xkb_state_unref(kb_state);
	xkb_keymap_unref(kb_map);
	kb_state = state;
	kb_map = map;
	repeat_kc = 0;
}

static void kb_enter(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *s, struct wl_array *keys)
{
}

static void kb_leave(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *s)
{
	repeat_kc = 0;
}

static void kb_key(void *data, struct wl_keyboard *k, uint32_t serial,
	uint32_t time, uint32_t code, uint32_t state)
{
	uint32_t kc = code + 8;
	if (!kb_state)
		return;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (kc == repeat_kc)
			repeat_kc = 0;
		return;
	}
	repeat_kc = 0;
	key_action(xkb_state_key_get_one_sym(kb_state, kc));
	if (repeat_rate > 0 && xkb_keymap_key_repeats(kb_map, kc) &&
	    key_repeats(kc)) {
		repeat_kc = kc;
		repeat_next = now_ms() + repeat_delay;
	}
}

static void kb_modifiers(void *data, struct wl_keyboard *k, uint32_t serial,
	uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
	if (kb_state)
		xkb_state_update_mask(kb_state, depressed, latched, locked, 0, 0,
			group);
}

static void kb_repeat_info(void *data, struct wl_keyboard *k, int32_t rate,
	int32_t delay)
{
	repeat_rate = rate;
	repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_modifiers,
	.repeat_info = kb_repeat_info,
};

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	ptr_surface = s;
	ptr_x = wl_fixed_to_int(x);
	ptr_y = wl_fixed_to_int(y);
	if (cursor_img) {
		wl_pointer_set_cursor(p, serial, cursor_surf, cursor_img->hotspot_x,
			cursor_img->hotspot_y);
		wl_surface_attach(cursor_surf,
			wl_cursor_image_get_buffer(cursor_img), 0, 0);
		wl_surface_damage_buffer(cursor_surf, 0, 0, cursor_img->width,
			cursor_img->height);
		wl_surface_commit(cursor_surf);
	}
}

/* Not every compositor keeps the pointer on a window while a button is held,
 * and then the release never reaches us: a drag ends at the window's edge. */
static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s)
{
	ptr_surface = NULL;
	end_drag();
	if (btn_pressed) {
		btn_pressed = 0;
		dirty = 1;
	}
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
	wl_fixed_t x, wl_fixed_t y)
{
	ptr_x = wl_fixed_to_int(x);
	ptr_y = wl_fixed_to_int(y);
	if (dragging)
		scale_to(ptr_x);
	if (btn_pressed && !in(r_btn, ptr_x, ptr_y)) {
		btn_pressed = 0;
		dirty = 1;
	}
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
	uint32_t time, uint32_t code, uint32_t state)
{
	if (code != BTN_LEFT || !ptr_surface)
		return;
	if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
		end_drag();
		if (btn_pressed) {
			btn_pressed = 0;
			dirty = 1;
			toggle_mute();
		}
		return;
	}
	if (sink_state != SINK_OK)
		return;
	if (in(r_btn, ptr_x, ptr_y)) {
		btn_pressed = 1;
		dirty = 1;
	} else if (in(r_scale, ptr_x, ptr_y)) {
		int tx = thumb_x();
		/* Grabbing the slider must not move it; elsewhere it jumps here. */
		if (ptr_x >= tx && ptr_x < tx + THUMB) {
			drag_off = ptr_x - tx;
		} else {
			drag_off = THUMB / 2;
			scale_to(ptr_x);
		}
		dragging = 1;
	}
}

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis, wl_fixed_t value)
{
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		axis_acc += wl_fixed_to_double(value);
}

static void ptr_axis_source(void *data, struct wl_pointer *p, uint32_t source)
{
}

static void ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis)
{
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		axis_acc = 0;
}

static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis,
	int32_t steps)
{
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		axis_steps += steps;
}

/* Wheels report steps; touchpads only a distance, of which AXIS_STEP makes
 * one wheel step. Up means negative, and louder. */
static void ptr_frame(void *data, struct wl_pointer *p)
{
	enum { AXIS_STEP = 15 };
	int steps = axis_steps;
	if (steps) {
		axis_acc = 0;
	} else {
		steps = (int)(axis_acc / AXIS_STEP);
		axis_acc -= steps * AXIS_STEP;
	}
	axis_steps = 0;
	if (steps && !dragging)
		step_volume(-steps * STEP);
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_button,
	.axis = ptr_axis,
	.frame = ptr_frame,
	.axis_source = ptr_axis_source,
	.axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	int want_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
	int want_kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	if (want_ptr && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	} else if (!want_ptr && pointer) {
		wl_pointer_release(pointer);
		pointer = NULL;
		ptr_surface = NULL;
	}
	if (want_kb && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	} else if (!want_kb && keyboard) {
		wl_keyboard_release(keyboard);
		keyboard = NULL;
		repeat_kc = 0;
	}
}

static void seat_name(void *data, struct wl_seat *s, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ---- window setup ------------------------------------------------------- */

static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial)
{
	xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { wm_ping };

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	if (!strcmp(iface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(r, name, &wl_compositor_interface,
			MIN(ver, 4));
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, MIN(ver, 7));
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface,
			MIN(ver, 2));
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
		deco_mgr = wl_registry_bind(r, name,
			&zxdg_decoration_manager_v1_interface, 1);
	}
}

static void registry_remove(void *data, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static void main_xdg_configure(void *data, struct xdg_surface *s,
	uint32_t serial)
{
	xdg_surface_ack_configure(s, serial);
	W = pend_w > 0 ? pend_w : float_w;
	H = pend_h > 0 ? pend_h : min_h;
	if (!pend_bound)
		float_w = W;
	configured = 1;
	need_commit = 1;
	dirty = 1;
}

static const struct xdg_surface_listener main_xdg_listener = {
	main_xdg_configure,
};

/* 0 leaves the size to us, which after maximizing is the size from before. */
static void top_configure(void *data, struct xdg_toplevel *t, int32_t w,
	int32_t h, struct wl_array *states)
{
	uint32_t *st;
	pend_w = w;
	pend_h = h;
	pend_bound = 0;
	wl_array_for_each(st, states)
		if (*st != XDG_TOPLEVEL_STATE_ACTIVATED &&
		    *st != XDG_TOPLEVEL_STATE_RESIZING)
			pend_bound = 1;
}

static void top_close(void *data, struct xdg_toplevel *t)
{
	exit(0);
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = top_configure,
	.close = top_close,
};

static void cursor_init(void)
{
	const char *s = getenv("XCURSOR_SIZE");
	int size = s && atoi(s) > 0 ? atoi(s) : 24;
	struct wl_cursor_theme *theme =
		wl_cursor_theme_load(getenv("XCURSOR_THEME"), size, shm);
	if (!theme)
		return;
	struct wl_cursor *c = wl_cursor_theme_get_cursor(theme, "default");
	if (!c)
		c = wl_cursor_theme_get_cursor(theme, "left_ptr");
	if (!c)
		return;
	cursor_img = c->images[0];
	cursor_surf = wl_compositor_create_surface(compositor);
}

static void setup_window(void)
{
	surface = wl_compositor_create_surface(compositor);
	main_xdg = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(main_xdg, &main_xdg_listener, NULL);
	toplevel = xdg_surface_get_toplevel(main_xdg);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_app_id(toplevel, "sc");
	xdg_toplevel_set_title(toplevel, "sc");
	xdg_toplevel_set_min_size(toplevel, 220, min_h);
	if (deco_mgr) {
		struct zxdg_toplevel_decoration_v1 *d =
			zxdg_decoration_manager_v1_get_toplevel_decoration(deco_mgr,
				toplevel);
		zxdg_toplevel_decoration_v1_set_mode(d,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

/* ---- main loop ---------------------------------------------------------- */

static void lost_connection(void)
{
	fprintf(stderr, "sc: lost the connection to the compositor: %s\n",
		strerror(wl_display_get_error(dpy)));
	exit(1);
}

static short wl_events, wl_revents;

/* Pulse waits here, and for the Wayland socket too. */
static int poll_both(struct pollfd *ufds, unsigned long nfds, int timeout,
	void *u)
{
	struct pollfd fds[nfds + 1];
	memcpy(fds, ufds, nfds * sizeof *fds);
	fds[nfds] = (struct pollfd){ .fd = wl_display_get_fd(dpy),
		.events = wl_events };
	int ret = poll(fds, nfds + 1, timeout);
	int saved = errno;
	if (ret >= 0) {
		for (unsigned long i = 0; i < nfds; i++)
			ufds[i].revents = fds[i].revents;
		wl_revents = fds[nfds].revents;
	}
	errno = saved;
	return ret;
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		int help = !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help");
		fprintf(stderr, "usage: sc\n"
			"Environment: SC_FONT (default \"Sans 10\"), "
			"MWC_INACTIVE, MWC_ACTIVE.\n");
		return help ? 0 : 1;
	}
	setlocale(LC_ALL, "");
	signal(SIGPIPE, SIG_IGN);       /* as libpulse asks of its programs */

	if (!(dpy = wl_display_connect(NULL))) {
		fprintf(stderr, "sc: cannot connect to a Wayland compositor\n");
		return 1;
	}
	if (!(xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) {
		fprintf(stderr, "sc: cannot set up xkbcommon\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(dpy);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!compositor || !shm || !wm_base) {
		fprintf(stderr, "sc: the compositor lacks wl_compositor, wl_shm "
			"or xdg_wm_base\n");
		return 1;
	}
	cursor_init();
	theme_init();
	setup_window();
	wl_surface_commit(surface);

	if (!(ml = pa_mainloop_new())) {
		fprintf(stderr, "sc: cannot set up the PulseAudio main loop\n");
		return 1;
	}
	pa_mainloop_set_poll_func(ml, poll_both, NULL);
	ctx_start();

	for (;;) {
		if (wl_display_dispatch_pending(dpy) < 0)
			lost_connection();
		uint64_t now = now_ms();
		if (ctx_failed)
			ctx_drop();
		else if (!ctx && now >= retry_at)
			ctx_start();
		if (repeat_kc && now >= repeat_next) {
			if (key_repeats(repeat_kc))
				key_action(xkb_state_key_get_one_sym(kb_state,
					repeat_kc));
			else
				repeat_kc = 0;
			repeat_next = now_ms() + MAX(1000 / MAX(repeat_rate, 1), 1);
		}
		/* A frame at a time, except that a configure wants its commit
		 * even if the window is hidden and gets no frames. */
		if (dirty && configured && (!frame || need_commit) && draw()) {
			dirty = 0;
			need_commit = 0;
		}

		if (wl_display_prepare_read(dpy) != 0)
			continue;       /* events arrived in the meantime */
		wl_events = POLLIN;
		if (wl_display_flush(dpy) < 0) {
			if (errno != EAGAIN) {
				wl_display_cancel_read(dpy);
				lost_connection();
			}
			wl_events |= POLLOUT;
		}
		int timeout = -1;
		if (!ctx || repeat_kc) {
			uint64_t wake = !ctx ? retry_at : repeat_next;
			if (!ctx && repeat_kc)
				wake = MIN(wake, repeat_next);
			now = now_ms();
			timeout = wake > now ? (int)MIN((wake - now) * 1000,
				(uint64_t)INT_MAX) : 0;
		}
		wl_revents = 0; /* Pulse skips the poll when it has work queued */
		if (pa_mainloop_prepare(ml, timeout) < 0 ||
		    (pa_mainloop_poll(ml) < 0 && errno != EINTR)) {
			wl_display_cancel_read(dpy);
			fprintf(stderr, "sc: the PulseAudio main loop failed\n");
			return 1;
		}
		if (wl_revents & (POLLIN | POLLERR | POLLHUP)) {
			if (wl_display_read_events(dpy) < 0)
				lost_connection();
		} else {
			wl_display_cancel_read(dpy);
		}
		if (pa_mainloop_dispatch(ml) < 0) {
			fprintf(stderr, "sc: the PulseAudio main loop failed\n");
			return 1;
		}
	}
}
