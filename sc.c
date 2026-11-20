/*
 * pulseControl - a minimal volume control for PulseAudio on Wayland, in the Motif look
 * of mwc (https://github.com/reygnn/mwc) and mfm.
 *
 * libpulse, libwayland, xkbcommon, Pango and Cairo, no toolkit. It
 * controls the default output and follows it when it changes, or when something
 * else changes its volume. It reads the same colours as mwc: MWC_INACTIVE /
 * MWC_ACTIVE from the environment, else Mwm*background and Mwm*activeBackground
 * from ~/.config/mwc/mwmrc, else the Motif defaults. The text takes
 * Mwm*foreground and Mwm*activeForeground, else black or white.
 *
 * Keys:   Up / Right / +  louder        Down / Left / -  quieter
 *         m / Space       mute          Esc / q / Ctrl+Q / Ctrl+W  quit
 *         digits          type the volume, Return takes it, Esc doesn't
 *         also the volume keys of the keyboard, while the window has the focus
 * Mouse:  click or drag the scale, wheel anywhere, click the speaker to mute,
 *         click the percentage to type there.
 *
 * The scale ends at 100 %. A volume above that (set by another program) is
 * shown and can be lowered, but pulseControl does not raise anything past 100 %.
 *
 * It asks for no server-side decoration and draws its own frame: only mwm's thin
 * border, no title bar, no buttons, no resize handles. The border takes the active
 * colour while it is the active window, as mwc's frames do.
 *
 * Everything runs in one thread: Pulse's main loop does the waiting, and its
 * poll function watches the Wayland socket as well.
 */
#define _GNU_SOURCE
#include <cairo.h>
#include <errno.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
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

#include "theme.h"
#include "util.h"
#include "version.h"
#include <xkbcommon/xkbcommon-keysyms.h>

#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define BEV 1           /* widgets' bevel, as mwc's buttons, wells and scrollbars */
#define FRAME_BEV 2     /* the frame's, as mwc's */
#define PAD 6
#define FRAME (FRAME_BEV + 1) /* mwm's border without resize handles, as mwc draws it */
#define FIELD_PAD 8     /* around the text in the volume field */
#define THUMB 30        /* width of the scale's slider */
#define STEP 5          /* percent per key press or wheel step */
#define RETRY_MS 1000   /* between attempts to reach the sound server */
#define MAX_SIZE 8192   /* a window's side, whatever a compositor asks */
#define MAX_STEPS 20    /* wheel steps in one frame */
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
/* Asked to go: the main loop ends after what the server was asked has gone, and
 * nothing read after the quit asks it anything more. */
static int quitting;

static int dragging, drag_off, btn_pressed;
/* Typing the volume. Fresh: what is shown goes at the first digit. */
static int editing, edit_fresh;
static char edit_buf[16];
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
static uint32_t seat_global;    /* its name in the registry */
static struct seats seats;      /* all announced, for when it goes */
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static struct wl_callback *frame;       /* a frame is on its way */
static struct shmbuf *bufs[2];
static struct xdg_surface *main_xdg;
static struct xdg_toplevel *toplevel;
static int configured;          /* the first configure has arrived */
static int buf_scale = 1;       /* buffer pixels per surface pixel */
static int need_commit;         /* a configure waits for its commit */
static int pend_w, pend_h;      /* size of the configure in progress */
static int pend_bound;          /* ... which is maximized, tiled or the like */
static int pend_activated;      /* ... and of the active window */

static struct wl_pointer *pointer;
static struct wl_surface *ptr_surface;
static int ptr_x, ptr_y;
static double axis_acc;
static int axis_steps;
static struct wl_surface *cursor_surf;
static struct wl_cursor_theme *cursor_theme;
static struct wl_cursor_image *cursor_img;
static int cursor_scale;         /* the window's scale it was loaded at */
static int cursor_buf_scale;     /* its surface's scale, which divides the image */
static struct wp_viewporter *viewporter;
static struct wp_viewport *cursor_vp;
static int cursor_vw, cursor_vh; /* its size on the window, if the viewport sets it */
static uint32_t ptr_serial;      /* of the last enter, to set the cursor again */
static void cursor_update(void);

static struct wl_keyboard *keyboard;
static struct xkb_context *xkb;
static struct xkb_keymap *kb_map;
static struct xkb_state *kb_state;
static uint32_t repeat_kc;      /* xkb keycode of the repeating key, or 0 */
static uint64_t repeat_next;
static int repeat_rate = 25, repeat_delay = 600;

static int W = 320, H, float_w = 320, float_h, min_w, min_h;
static int activated;           /* the active window: the active frame */
static struct palette pal;
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
	int fd = memfd_create("pulseControl", MFD_CLOEXEC);
	void *data = MAP_FAILED;
	if (b && fd >= 0 && ftruncate(fd, size) == 0)
		data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "pulseControl: no memory for a %dx%d buffer: %s\n", w, h,
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

static int mwmrc_color(const char *res, uint32_t *out)
{
	char v[256];
	return parse_color(mwmrc_get(res, v, sizeof v), out);
}

static uint32_t setting(const char *env, const char *res, uint32_t def)
{
	uint32_t rgb;
	if (parse_color(getenv(env), &rgb) || mwmrc_color(res, &rgb))
		return rgb;
	return def;
}

/* The dock's font, the same in deskControl, pulseControl, osDisplay and
 * digitalClock, and fixed, so that the four are always as high as each other. */
#define DOCK_FONT_FAMILY "DejaVu Sans"
#define DOCK_FONT_SIZE 10       /* points */

static PangoFontDescription *dock_font(PangoContext *pctx)
{
	PangoFontDescription *d = pango_font_description_new();
	pango_font_description_set_family(d, DOCK_FONT_FAMILY);
	pango_font_description_set_style(d, PANGO_STYLE_NORMAL);
	pango_font_description_set_variant(d, PANGO_VARIANT_NORMAL);
	pango_font_description_set_weight(d, PANGO_WEIGHT_NORMAL);
	pango_font_description_set_stretch(d, PANGO_STRETCH_NORMAL);
	pango_font_description_set_size(d, DOCK_FONT_SIZE * PANGO_SCALE);
	/* Without it fontconfig quietly takes another font, and the dock may not line
	 * up; say so. */
	PangoFont *pf = pango_context_load_font(pctx, d);
	PangoFontDescription *got = pf ? pango_font_describe(pf) : NULL;
	const char *fam = got ? pango_font_description_get_family(got) : NULL;
	if (!fam || g_ascii_strcasecmp(fam, DOCK_FONT_FAMILY))
		fprintf(stderr, "pulseControl: font %s not found, using %s\n",
			DOCK_FONT_FAMILY, fam ? fam : "none");
	if (got)
		pango_font_description_free(got);
	if (pf)
		g_object_unref(pf);
	return d;
}

static void theme_init(void)
{
	uint32_t fg, sel_fg;
	int has_fg = mwmrc_color("foreground", &fg);
	int has_sel_fg = mwmrc_color("activeForeground", &sel_fg);
	palette(&pal, setting("MWC_INACTIVE", "background", 0xaeb2c3),
		setting("MWC_ACTIVE", "activeBackground", 0x7387ad),
		has_fg ? &fg : NULL, has_sel_fg ? &sel_fg : NULL);

	PangoContext *pctx = pango_font_map_create_context(
		pango_cairo_font_map_get_default());
	PangoFontDescription *desc = dock_font(pctx);
	plo = pango_layout_new(pctx);
	pango_layout_set_font_description(plo, desc);
	pango_layout_set_single_paragraph_mode(plo, TRUE);
	/* Left to right as a paragraph, as in deskControl: text() places the text at
	 * the left of its box, and a right-to-left sink name would otherwise be flushed
	 * to its right. */
	pango_layout_set_auto_dir(plo, FALSE);
	PangoFontMetrics *m = pango_context_get_metrics(pctx, desc, NULL);
	font_asc = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(m));
	font_desc = PANGO_PIXELS_CEIL(pango_font_metrics_get_descent(m));
	pango_font_metrics_unref(m);
	pango_font_description_free(desc);
	g_object_unref(pctx);   /* the layout holds it */
	barh = font_asc + font_desc + 10;
	min_h = H = float_h = 2 * (FRAME + PAD) + 2 * barh + 4;
}

/* ---- PulseAudio --------------------------------------------------------- */

static void sink_info(pa_context *c, const pa_sink_info *i, int eol, void *u)
{
	if (eol > 0 || pending || dragging)
		return;
	dirty = 1;
	if (eol < 0 || !i) {
		sink_state = SINK_NONE;
		editing = 0;
		return;
	}
	pa_volume_t vol = pa_cvolume_max(&i->volume);
	if (vol != PA_VOLUME_MUTED || sink_state != SINK_OK ||
	    i->index != sink_idx || i->volume.channels != sink_shape.channels)
		sink_shape = i->volume;
	sink_idx = i->index;
	sink_mute = i->mute;
	sink_pct = ((uint64_t)vol * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM;
	sink_state = SINK_OK;
	/* Pango takes valid UTF-8 only: filtered, then cut between characters. */
	char *d = pa_utf8_filter(i->description ? i->description : i->name);
	size_t n = d ? strlen(d) : 0;
	if (n >= sizeof sink_desc) {
		n = sizeof sink_desc - 1;
		while (n > 0 && ((unsigned char)d[n] & 0xc0) == 0x80)
			n--;
	}
	memcpy(sink_desc, d ? d : "", n);
	sink_desc[n] = 0;
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

/* The window shows a change only once it is on its way: a context that has just
 * failed sends nothing, and ctx_drop resets the window a moment later. */
static void set_percent(int pct)
{
	if (sink_state != SINK_OK || pct == sink_pct || quitting)
		return;
	pa_cvolume cv = sink_shape;
	pa_cvolume_scale(&cv, ((uint64_t)pct * PA_VOLUME_NORM + 50) / 100);
	pa_operation *op = pa_context_set_sink_volume_by_index(ctx, sink_idx, &cv,
		set_done, NULL);
	if (!op)
		return;
	sink_pct = pct;
	dirty = 1;
	track(op);
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
	if (sink_state != SINK_OK || quitting)
		return;
	pa_operation *op = pa_context_set_sink_mute_by_index(ctx, sink_idx, !sink_mute,
		set_done, NULL);
	if (!op)
		return;
	sink_mute = !sink_mute;
	dirty = 1;
	track(op);
}

static void ctx_event(pa_context *c, pa_subscription_event_type_t t, uint32_t idx,
	void *u)
{
	/* Another sink's volume, or a stream starting on it, is none of ours. A new
	 * default comes as a server event. */
	if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK &&
	    sink_state == SINK_OK && idx != sink_idx)
		return;
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
	ctx = pa_context_new(pa_mainloop_get_api(ml), "pulseControl");
	if (!ctx) {
		fprintf(stderr, "pulseControl: cannot set up a PulseAudio context\n");
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
	editing = 0;
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

static void bevel_in(struct rect r, int w, uint32_t t, uint32_t b)
{
	for (int i = 0; i < w; i++) {
		fill(t, r.x + i, r.y + i, r.w - 2 * i, 1);
		fill(t, r.x + i, r.y + i, 1, r.h - 2 * i);
		fill(b, r.x + i + 1, r.y + r.h - 1 - i, r.w - 2 * i - 1, 1);
		fill(b, r.x + r.w - 1 - i, r.y + i + 1, 1, r.h - 2 * i - 1);
	}
}

static void bevel(struct rect r, int raised)
{
	bevel_in(r, BEV, raised ? pal.top : pal.bot, raised ? pal.bot : pal.top);
}

/* mwm's thin border: a raised bevel and one line of the face inside it. */
static void draw_frame(void)
{
	uint32_t face = activated ? pal.sel : pal.bg;
	fill(face, 0, 0, W, FRAME);
	fill(face, 0, H - FRAME, W, FRAME);
	fill(face, 0, 0, FRAME, H);
	fill(face, W - FRAME, 0, FRAME, H);
	bevel_in((struct rect){ 0, 0, W, H }, FRAME_BEV, mix(face, 0xffffff, 0.55f),
		mix(face, 0x000000, 0.5f));
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
	pw += 2 * BEV + FIELD_PAD;
	int in = FRAME + PAD;   /* everything sits inside the frame */
	r_btn = (struct rect){ in, in, barh, barh };
	r_pct = (struct rect){ W - in - pw, in, pw, barh };
	r_scale = (struct rect){ in + barh + 4, in,
		W - 2 * in - barh - pw - 12, barh };
	r_status = (struct rect){ in, H - in - barh, W - 2 * in, barh };
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
	set_rgb(sink_state == SINK_OK ? pal.fg : pal.dim);
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
		set_rgb(pal.err);
		cairo_set_line_width(cr, 2);
		cairo_move_to(cr, wx, cy - a + 1);
		cairo_line_to(cr, wx + a + 1, cy + a);
		cairo_move_to(cr, wx + a + 1, cy - a + 1);
		cairo_line_to(cr, wx, cy + a);
		cairo_stroke(cr);
	} else {
		fill(sink_state == SINK_OK ? pal.fg : pal.dim, wx, cy - a / 2, 1,
			a + 1);
		fill(sink_state == SINK_OK ? pal.fg : pal.dim, wx + 2, cy - a, 1,
			2 * a + 1);
	}
}

static int in(struct rect r, int x, int y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* As a Motif push button: in while the press is held over it, out while the pointer
 * is off it, and in again when it comes back. */
static int btn_down(void)
{
	return btn_pressed && in(r_btn, ptr_x, ptr_y);
}

static void paint(void)
{
	char s[32];
	int ok = sink_state == SINK_OK;
	layout();
	fill(pal.bg, 0, 0, W, H);
	draw_frame();

	/* mute button: stays in while muted */
	int down = (ok && sink_mute) != btn_down();
	bevel(r_btn, !down);
	draw_speaker(r_btn, ok && sink_mute, down);

	/* scale */
	int tx = thumb_x(), ix = r_scale.x + BEV, iy = r_scale.y + BEV;
	int iw = r_scale.w - 2 * BEV, ih = r_scale.h - 2 * BEV;
	fill(pal.trough, ix, iy, iw, ih);
	if (ok && !sink_mute)
		fill(pal.sel, ix, iy, tx - ix, ih);
	bevel(r_scale, 0);
	if (ok) {
		struct rect sl = { tx, iy, THUMB, ih };
		fill(pal.bg, sl.x, sl.y, sl.w, sl.h);
		bevel(sl, 1);
		fill(pal.bot, tx + THUMB / 2 - 1, iy + BEV, 1, ih - 2 * BEV);
		fill(pal.top, tx + THUMB / 2, iy + BEV, 1, ih - 2 * BEV);
	}

	/* volume field */
	int fx = r_pct.x + BEV, fy = r_pct.y + BEV;
	int fw = r_pct.w - 2 * BEV, fh = r_pct.h - 2 * BEV;
	fill(editing ? pal.paper : pal.list, fx, fy, fw, fh);
	bevel(r_pct, 0);
	if (editing) {
		/* Nothing typed yet: the volume as it is now, also after another
		 * program or a volume key changed it. */
		if (edit_fresh)
			snprintf(edit_buf, sizeof edit_buf, "%d", sink_pct);
		snprintf(s, sizeof s, "%s %%", edit_buf);
		int tw = textw(s), tpos = fx + fw - FIELD_PAD / 2 - tw;
		if (edit_fresh) {
			fill(pal.sel, tpos - 1, fy + 1, tw + 2, fh - 2);
		} else {
			fill(0x000000, tpos + textw(edit_buf), fy + 2, 1, fh - 4);
		}
		text(edit_fresh ? pal.sel_fg : 0x000000, tpos, r_pct.y, r_pct.h,
			fw, s);
	} else if (ok) {
		snprintf(s, sizeof s, "%d %%", sink_pct);
		text(sink_mute ? pal.list_dim : pal.list_fg,
			fx + fw - FIELD_PAD / 2 - textw(s),
			r_pct.y, r_pct.h, fw, s);
	}

	/* status line */
	bevel(r_status, 0);
	int sx = r_status.x + BEV + 4, sw = r_status.w - 2 * BEV - 8;
	if (ok)
		text(pal.fg, sx, r_status.y, r_status.h, sw, sink_desc);
	else if (sink_state == SINK_NONE)
		text(pal.err, sx, r_status.y, r_status.h, sw, "no output device");
	else if (!ctx || pa_context_get_state(ctx) != PA_CONTEXT_READY)
		text(retry_at ? pal.err : pal.dim, sx, r_status.y, r_status.h, sw,
			retry_at ? "no sound server" : "connecting ...");
}

/* 0 if the compositor holds both buffers: try again after a release. */
static int draw(void)
{
	/* A huge window gets fewer pixels, not an error. */
	int sc = draw_scale(W, H, buf_scale);
	struct shmbuf *b = get_buffer(W * sc, H * sc);
	if (!b)
		return 0;
	cr = b->cr;
	cairo_identity_matrix(cr);
	cairo_scale(cr, sc, sc);
	/* Glyphs hinted and placed for the buffer's pixels, not the surface's. */
	pango_cairo_update_layout(cr, plo);
	paint();
	cairo_surface_flush(b->cs);
	if (frame)
		wl_callback_destroy(frame);
	frame = wl_surface_frame(surface);
	wl_callback_add_listener(frame, &frame_listener, NULL);
	wl_surface_set_buffer_scale(surface, sc);
	wl_surface_attach(surface, b->wl, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, b->w, b->h);
	wl_surface_commit(surface);
	b->busy = 1;
	return 1;
}

/* ---- input -------------------------------------------------------------- */

static void scale_to(int x)
{
	int p = ((x - drag_off - r_scale.x - BEV) * 100 + scale_travel() / 2) /
		scale_travel();
	/* The slider's end is not the volume, if that is above 100 %. */
	if (sink_pct > 100 && p >= 100)
		return;
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

static int mod_on(const char *name)
{
	return xkb_state_mod_name_is_active(kb_state, name,
		XKB_STATE_MODS_EFFECTIVE) > 0;
}

/* Alt, Super and Ctrl make a key mean something else: Alt+Q must not quit.
 * Shift doesn't, as + takes it on many layouts. */
static int plain_key(void)
{
	return !mod_on(XKB_MOD_NAME_ALT) && !mod_on(XKB_MOD_NAME_LOGO) &&
		!mod_on(XKB_MOD_NAME_CTRL);
}

/* The digit of a key, or -1. */
static int key_digit(xkb_keysym_t ks)
{
	if (ks >= XKB_KEY_0 && ks <= XKB_KEY_9)
		return ks - XKB_KEY_0;
	if (ks >= XKB_KEY_KP_0 && ks <= XKB_KEY_KP_9)
		return ks - XKB_KEY_KP_0;
	return -1;
}

static void edit_begin(void)
{
	if (sink_state != SINK_OK || editing)
		return;
	editing = edit_fresh = 1;
	snprintf(edit_buf, sizeof edit_buf, "%d", sink_pct);
	dirty = 1;
}

static void edit_cancel(void)
{
	if (editing) {
		editing = 0;
		dirty = 1;
	}
}

/* Nothing typed changes nothing. A typed volume is meant to be heard, so one above
 * zero ends the muting, unlike a step. */
static void edit_commit(void)
{
	int typed = !edit_fresh && edit_buf[0];
	int pct = atoi(edit_buf);
	edit_cancel();
	if (!typed)
		return;
	set_percent(pct);
	if (pct > 0 && sink_mute)
		toggle_mute();
}

/* Keeps to 0..100: a digit that would go past it doesn't arrive. */
static void edit_digit(int d)
{
	char next[sizeof edit_buf];
	size_t n = edit_fresh ? 0 : strlen(edit_buf);
	if (n == 1 && edit_buf[0] == '0')
		n = 0;          /* not "05" */
	if (n >= 3)
		return;
	memcpy(next, edit_buf, n);
	next[n] = (char)('0' + d);
	next[n + 1] = 0;
	if (atoi(next) > 100)
		return;
	memcpy(edit_buf, next, n + 2);
	edit_fresh = 0;
	dirty = 1;
}

static void edit_key(xkb_keysym_t ks)
{
	int d = key_digit(ks);
	if (d >= 0) {
		edit_digit(d);
		return;
	}
	switch (ks) {
	case XKB_KEY_BackSpace: {
		size_t n = edit_fresh ? 0 : strlen(edit_buf);
		edit_buf[n ? n - 1 : 0] = 0;
		edit_fresh = 0;
		dirty = 1;
		break;
	}
	case XKB_KEY_Return: case XKB_KEY_KP_Enter:
		edit_commit();
		break;
	case XKB_KEY_Escape:
		edit_cancel();
		break;
	}
}

static void key_action(xkb_keysym_t ks)
{
	switch (ks) {       /* the keyboard's volume keys mean only that */
	case XKB_KEY_XF86AudioRaiseVolume:
		step_volume(STEP);
		return;
	case XKB_KEY_XF86AudioLowerVolume:
		step_volume(-STEP);
		return;
	case XKB_KEY_XF86AudioMute:
		toggle_mute();
		return;
	}
	if (mod_on(XKB_MOD_NAME_CTRL) && !mod_on(XKB_MOD_NAME_ALT) &&
	    !mod_on(XKB_MOD_NAME_LOGO) &&
	    (ks == XKB_KEY_q || ks == XKB_KEY_Q || ks == XKB_KEY_w ||
	     ks == XKB_KEY_W)) {
		quitting = 1;
		return;
	}
	if (!plain_key())
		return;
	if (editing) {
		edit_key(ks);
		return;
	}
	if (key_digit(ks) >= 0) {
		edit_begin();
		if (editing)
			edit_key(ks);
		return;
	}
	switch (ks) {
	case XKB_KEY_Up: case XKB_KEY_KP_Up:
	case XKB_KEY_Right: case XKB_KEY_KP_Right:
	case XKB_KEY_plus: case XKB_KEY_KP_Add:
		step_volume(STEP);
		break;
	case XKB_KEY_Down: case XKB_KEY_KP_Down:
	case XKB_KEY_Left: case XKB_KEY_KP_Left:
	case XKB_KEY_minus: case XKB_KEY_KP_Subtract:
		step_volume(-STEP);
		break;
	case XKB_KEY_m: case XKB_KEY_M: case XKB_KEY_space:
		toggle_mute();
		break;
	case XKB_KEY_q: case XKB_KEY_Q: case XKB_KEY_Escape:
		quitting = 1;
		break;
	}
}

/* Only the volume repeats when a key is held. */
static int key_repeats(uint32_t kc)
{
	switch (xkb_state_key_get_one_sym(kb_state, kc)) {
	case XKB_KEY_XF86AudioRaiseVolume:
	case XKB_KEY_XF86AudioLowerVolume:
		return 1;
	case XKB_KEY_Up: case XKB_KEY_KP_Up:
	case XKB_KEY_Right: case XKB_KEY_KP_Right:
	case XKB_KEY_plus: case XKB_KEY_KP_Add:
	case XKB_KEY_Down: case XKB_KEY_KP_Down:
	case XKB_KEY_Left: case XKB_KEY_KP_Left:
	case XKB_KEY_minus: case XKB_KEY_KP_Subtract:
		return plain_key() && !editing;
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
	/* The protocol doesn't promise a NUL at the end, and reading for one could
	 * run past the mapping. */
	struct xkb_keymap *map = xkb_keymap_new_from_buffer(xkb, s, strnlen(s, size),
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
	edit_cancel();
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
	/* Pressing a key that doesn't repeat, a modifier, leaves the repeat alone.
	 * Shift held keeps it going; with Ctrl, Alt or Super held key_repeats() is 0
	 * at the next step, and the repeat ends until the key is pressed again. */
	int repeats = xkb_keymap_key_repeats(kb_map, kc);
	if (repeats)
		repeat_kc = 0;
	key_action(xkb_state_key_get_one_sym(kb_state, kc));
	if (repeats && repeat_rate > 0 && key_repeats(kc)) {
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
	ptr_serial = serial;
	cursor_update();
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
	int old_x = ptr_x, was_down = btn_down();
	ptr_x = wl_fixed_to_int(x);
	ptr_y = wl_fixed_to_int(y);
	/* Only sideways: a hand's jitter must not change a grabbed slider. */
	if (dragging && ptr_x != old_x)
		scale_to(ptr_x);
	if (btn_down() != was_down)
		dirty = 1;
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
	uint32_t time, uint32_t code, uint32_t state)
{
	if (code != BTN_LEFT || !ptr_surface)
		return;
	if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
		end_drag();
		/* A click counts on the release, over the button it went down on. */
		if (btn_pressed) {
			int take = btn_down();
			btn_pressed = 0;
			dirty = 1;
			if (take)
				toggle_mute();
		}
		return;
	}
	if (sink_state != SINK_OK)
		return;
	if (in(r_pct, ptr_x, ptr_y)) {
		edit_begin();
		return;
	}
	edit_cancel();
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

static void ptr_frame(void *data, struct wl_pointer *p);

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis, wl_fixed_t value)
{
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return;
	axis_acc += wl_fixed_to_double(value);
	/* Before wl_seat 5 no frame ends the event: each one counts by itself. */
	if (wl_pointer_get_version(p) < WL_POINTER_FRAME_SINCE_VERSION)
		ptr_frame(data, p);
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
		axis_steps = MAX(MIN(axis_steps + MAX(MIN(steps, MAX_STEPS), -MAX_STEPS),
			MAX_STEPS), -MAX_STEPS);
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
		double q = axis_acc / AXIS_STEP;
		steps = (int)MAX(MIN(q, MAX_STEPS), -MAX_STEPS);
		axis_acc = fabs(q) > MAX_STEPS ? 0 : axis_acc - steps * AXIS_STEP;
	}
	axis_steps = 0;
	if (steps && !dragging) {
		edit_cancel();
		step_volume(-steps * STEP);
	}
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
		end_drag();
		btn_pressed = 0;
		dirty = 1;
		/* release is from wl_seat 3 on; an older seat only knows destroy. */
		if (wl_pointer_get_version(pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
			wl_pointer_release(pointer);
		else
			wl_pointer_destroy(pointer);
		pointer = NULL;
		ptr_surface = NULL;
	}
	if (want_kb && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	} else if (!want_kb && keyboard) {
		if (wl_keyboard_get_version(keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
			wl_keyboard_release(keyboard);
		else
			wl_keyboard_destroy(keyboard);
		keyboard = NULL;
		repeat_kc = 0;
		edit_cancel();  /* gone without a leave, perhaps */
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

static void seat_bind(struct wl_registry *r, uint32_t name, uint32_t ver)
{
	seat = wl_registry_bind(r, name, &wl_seat_interface, MIN(ver, 7));
	seat_global = name;
	wl_seat_add_listener(seat, &seat_listener, NULL);
}

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	/* wl_surface.damage_buffer is from version 4: an older one is not taken. */
	if (!strcmp(iface, wl_compositor_interface.name) && ver >= 4) {
		compositor = wl_registry_bind(r, name, &wl_compositor_interface,
			MIN(ver, 6));
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_seat_interface.name)) {
		if (seats_add(&seats, name, ver) && !seat)
			seat_bind(r, name, ver);
	} else if (!strcmp(iface, wp_viewporter_interface.name) && !viewporter) {
		viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface,
			MIN(ver, 2));
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	}
}

/* A seat that goes is let go of, and the oldest one left taken; with none, the
 * next one announced. */
static void registry_remove(void *data, struct wl_registry *r, uint32_t name)
{
	seats_remove(&seats, name);
	if (!seat || name != seat_global)
		return;
	seat_capabilities(NULL, seat, 0);
	if (wl_seat_get_version(seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
		wl_seat_release(seat);
	else
		wl_seat_destroy(seat);
	seat = NULL;
	unsigned next, ver;
	if (seats_next(&seats, &next, &ver))
		seat_bind(r, next, ver);
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static void main_xdg_configure(void *data, struct xdg_surface *s,
	uint32_t serial)
{
	xdg_surface_ack_configure(s, serial);
	/* Not below the minimum, which a tiling compositor may ignore: the scale
	 * would have no room. */
	W = MIN(MAX(pend_w > 0 ? pend_w : float_w, min_w), MAX_SIZE);
	H = MIN(MAX(pend_h > 0 ? pend_h : float_h, min_h), MAX_SIZE);
	if (!pend_bound) {
		float_w = W;
		float_h = H;
	}
	/* By the state, not the keyboard: mwc keeps a window active while a layer
	 * surface such as a launcher has the keys, and draws its frames so. */
	activated = pend_activated;
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
	pend_activated = 0;
	wl_array_for_each(st, states)
		if (*st == XDG_TOPLEVEL_STATE_ACTIVATED)
			pend_activated = 1;
		else if (*st != XDG_TOPLEVEL_STATE_RESIZING)
			pend_bound = 1;
}

static void top_close(void *data, struct xdg_toplevel *t)
{
	quitting = 1;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = top_configure,
	.close = top_close,
};

/* The buffer scale and viewport for the loaded image. A theme has only some
 * sizes: Adwaita gives 30 for 32. A buffer scale that doesn't divide the image is
 * a protocol error. With a viewport the buffer is shown at its size over the
 * scale, as mwc shows its own cursor; without, at the largest scale up to the
 * window's that divides it, and that much larger. */
static void cursor_fit(void)
{
	int w = cursor_img->width, h = cursor_img->height, sc = buf_scale;
	cursor_buf_scale = 1;
	cursor_vw = cursor_vh = -1;
	if (w % sc == 0 && h % sc == 0) {
		cursor_buf_scale = sc;
	} else if (cursor_vp) {
		cursor_vw = MAX((w + sc / 2) / sc, 1);
		cursor_vh = MAX((h + sc / 2) / sc, 1);
	} else {
		for (int d = sc - 1; d > 1; d--)
			if (w % d == 0 && h % d == 0) {
				cursor_buf_scale = d;
				break;
			}
	}
}

/* The cursor on the window, while the pointer is over it; 0 if not shown. */
static int cursor_show(void)
{
	if (!cursor_img || !pointer || !ptr_surface)
		return 0;
	int hx = cursor_img->hotspot_x / cursor_buf_scale;
	int hy = cursor_img->hotspot_y / cursor_buf_scale;
	if (cursor_vw > 0) {
		/* Rounded as the viewport's size is. */
		hx = (cursor_img->hotspot_x * cursor_vw + cursor_img->width / 2) /
			cursor_img->width;
		hy = (cursor_img->hotspot_y * cursor_vh + cursor_img->height / 2) /
			cursor_img->height;
	}
	wl_pointer_set_cursor(pointer, ptr_serial, cursor_surf, hx, hy);
	if (cursor_vp)
		wp_viewport_set_destination(cursor_vp, cursor_vw, cursor_vh);
	wl_surface_set_buffer_scale(cursor_surf, cursor_buf_scale);
	wl_surface_attach(cursor_surf, wl_cursor_image_get_buffer(cursor_img), 0, 0);
	wl_surface_damage_buffer(cursor_surf, 0, 0, cursor_img->width,
		cursor_img->height);
	wl_surface_commit(cursor_surf);
	return 1;
}

/* The theme's cursor at the window's scale, loaded again when that changes, and
 * shown if the pointer is on the window. The new theme first: a failed load keeps
 * the old cursor and is tried again on the next enter. The old theme's buffers go
 * with it, so the surface shows the new cursor or none before it goes. */
static void cursor_update(void)
{
	if (cursor_scale != buf_scale) {
		/* A whole number only: "32x" is none. */
		const char *s = getenv("XCURSOR_SIZE");
		char *end = NULL;
		long size = s ? strtol(s, &end, 10) : 0;
		if (!s || end == s || *end || size <= 0)
			size = 24;
		/* No theme has more; and times the scale it must stay an int. */
		size = MIN(size, 256);
		struct wl_cursor_theme *t = wl_cursor_theme_load(getenv("XCURSOR_THEME"),
			(int)size * buf_scale, shm);
		struct wl_cursor *c = t ? wl_cursor_theme_get_cursor(t, "default") : NULL;
		if (t && !c)
			c = wl_cursor_theme_get_cursor(t, "left_ptr");
		if (c) {
			struct wl_cursor_theme *old = cursor_theme;
			cursor_theme = t;
			cursor_img = c->images[0];
			cursor_scale = buf_scale;
			if (!cursor_surf)
				cursor_surf = wl_compositor_create_surface(compositor);
			if (viewporter && !cursor_vp)
				cursor_vp = wp_viewporter_get_viewport(viewporter, cursor_surf);
			cursor_fit();
			if (!cursor_show() && old) {
				wl_surface_attach(cursor_surf, NULL, 0, 0);
				wl_surface_commit(cursor_surf);
			}
			if (old)
				wl_cursor_theme_destroy(old);
			return;
		}
		if (t)
			wl_cursor_theme_destroy(t);
	}
	cursor_show();
}

/* Sharp on a scaled output: wl_compositor 6 says which scale the output it is on
 * has. An older compositor never does, and it stays at 1. */
static void surf_enter(void *data, struct wl_surface *s, struct wl_output *o)
{
}

static void surf_leave(void *data, struct wl_surface *s, struct wl_output *o)
{
}

static void surf_preferred_scale(void *data, struct wl_surface *s, int32_t factor)
{
	factor = MIN(factor, 16);       /* no output is denser */
	if (factor < 1 || factor == buf_scale)
		return;
	buf_scale = factor;
	dirty = 1;
	/* Not only at the next enter: the pointer may stay on the window. */
	cursor_update();
}

static void surf_preferred_transform(void *data, struct wl_surface *s,
	uint32_t transform)
{
}

static const struct wl_surface_listener surf_listener = {
	.enter = surf_enter,
	.leave = surf_leave,
	.preferred_buffer_scale = surf_preferred_scale,
	.preferred_buffer_transform = surf_preferred_transform,
};

static void setup_window(void)
{
	surface = wl_compositor_create_surface(compositor);
	wl_surface_add_listener(surface, &surf_listener, NULL);
	main_xdg = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(main_xdg, &main_xdg_listener, NULL);
	toplevel = xdg_surface_get_toplevel(main_xdg);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_app_id(toplevel, "pulseControl");
	xdg_toplevel_set_title(toplevel, "pulseControl");
	/* Room for all of it with the font in use, and a pixel per percent. */
	min_w = MAX(220, 2 * (FRAME + PAD) + barh + textw("100 %") + 2 * BEV + FIELD_PAD + 12 +
		2 * BEV + THUMB + 100);
	float_w = W = MAX(float_w, min_w);
	xdg_toplevel_set_min_size(toplevel, min_w, min_h);
}

/* ---- main loop ---------------------------------------------------------- */

static void lost_connection(void)
{
	fprintf(stderr, "pulseControl: lost the connection to the compositor: %s\n",
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
	/* Going, only Pulse counts: a compositor that has hung up would wake the
	 * poll at once, again and again. */
	int ret = poll(fds, nfds + !quitting, timeout);
	int saved = errno;
	if (ret >= 0) {
		for (unsigned long i = 0; i < nfds; i++)
			ufds[i].revents = fds[i].revents;
		wl_revents = fds[nfds].revents;
	}
	errno = saved;
	return ret;
}

/* A step and a quit read in one batch of events: the step is only queued in
 * libpulse, and written in the next dispatch. Before going, that and its answer
 * are waited for, a second at most. */
static void quit(void)
{
	uint64_t end = now_ms() + 1000, now;
	while (ctx && pa_context_get_state(ctx) == PA_CONTEXT_READY &&
	       pa_context_is_pending(ctx) && (now = now_ms()) < end) {
		if (pa_mainloop_prepare(ml, (int)((end - now) * 1000)) < 0 ||
		    (pa_mainloop_poll(ml) < 0 && errno != EINTR) ||
		    pa_mainloop_dispatch(ml) < 0)
			break;
	}
	exit(0);
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		int version = argc == 2 && !strcmp(argv[1], "-v");
		int help = argc == 2 && !strcmp(argv[1], "-h");
		if (version)
			printf("pulseControl %s\n", PULSECONTROL_VERSION);
		else    /* asked for, it goes to stdout, as mwc's does */
			fprintf(help ? stdout : stderr, "usage: pulseControl [-v]\n"
				"Environment: MWC_INACTIVE, MWC_ACTIVE.\n");
		/* A full disk or a closed stdout is no success: fully buffered, the
		 * write only fails here. */
		if (fflush(stdout) == EOF || ferror(stdout)) {
			fprintf(stderr, "pulseControl: stdout: %s\n", strerror(errno));
			return 1;
		}
		return version || help ? 0 : 1;
	}
	setlocale(LC_ALL, "");
	signal(SIGPIPE, SIG_IGN);       /* as libpulse asks of its programs */

	if (!(dpy = wl_display_connect(NULL))) {
		fprintf(stderr, "pulseControl: cannot connect to a Wayland compositor\n");
		return 1;
	}
	if (!(xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) {
		fprintf(stderr, "pulseControl: cannot set up xkbcommon\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(dpy);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(dpy) < 0)
		lost_connection();
	if (!compositor || !shm || !wm_base) {
		fprintf(stderr, "pulseControl: the compositor lacks wl_compositor 4, "
			"wl_shm or xdg_wm_base\n");
		return 1;
	}
	cursor_update();
	theme_init();
	setup_window();
	wl_surface_commit(surface);

	if (!(ml = pa_mainloop_new())) {
		fprintf(stderr, "pulseControl: cannot set up the PulseAudio main loop\n");
		return 1;
	}
	pa_mainloop_set_poll_func(ml, poll_both, NULL);
	ctx_start();

	for (;;) {
		if (wl_display_dispatch_pending(dpy) < 0)
			lost_connection();
		if (quitting)
			quit();
		uint64_t now = now_ms();
		if (!ctx && now >= retry_at)
			ctx_start();
		/* Also right after a start that failed at once: the wait below
		 * needs to see no context, to wake for the next attempt. */
		if (ctx_failed)
			ctx_drop();
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
			/* EPIPE: the compositor has closed, but may have said why first.
			 * Reading that error makes libwayland report it. */
			if (errno != EAGAIN && errno != EPIPE) {
				wl_display_cancel_read(dpy);
				lost_connection();
			}
			if (errno == EAGAIN)
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
			fprintf(stderr, "pulseControl: the PulseAudio main loop failed\n");
			return 1;
		}
		if (wl_revents & (POLLIN | POLLERR | POLLHUP)) {
			if (wl_display_read_events(dpy) < 0)
				lost_connection();
		} else {
			wl_display_cancel_read(dpy);
		}
		if (pa_mainloop_dispatch(ml) < 0) {
			fprintf(stderr, "pulseControl: the PulseAudio main loop failed\n");
			return 1;
		}
	}
}
