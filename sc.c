/*
 * pulseControl - a minimal volume control for PulseAudio on Wayland, in the Motif look
 * of mwc (https://github.com/reygnn/mwc) and mfm.
 *
 * libpulse and mtk (mtk/), the toolkit of mwc's clients, which brings the window,
 * the seat, the cursor, the colours and the bevels. It controls the default output
 * and follows it when it changes, or when something else changes its volume. It
 * reads the same colours as mwc: MWC_INACTIVE / MWC_ACTIVE from the environment,
 * else Mwm*background and Mwm*activeBackground from ~/.config/mwc/mwmrc, else the
 * Motif defaults. The text takes Mwm*foreground and Mwm*activeForeground, else
 * black or white as Motif picks it.
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
 * It asks for no server-side decoration and draws only mwm's thin border, which
 * takes the active colour while it is the active window, as mwc's frames do.
 *
 * Everything runs in one thread: Pulse's main loop does the waiting, and its
 * poll function watches mtk's Wayland socket as well.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <poll.h>
#include <pulse/pulseaudio.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "mtk.h"
#include "theme.h"
#include "version.h"

#define BEV 1           /* widgets' bevel, as mwc's buttons, wells and scrollbars */
#define PAD 6
#define FRAME MTK_BORDER        /* mwm's border without resize handles */
#define FIELD_PAD 8     /* around the text in the volume field */
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
/* Asked to go: the main loop ends after what the server was asked has gone, and
 * nothing read after the quit asks it anything more. */
static int quitting;

static int dragging, drag_off, btn_pressed;
/* Typing the volume. Fresh: what is shown goes at the first digit. */
static int editing, edit_fresh;
static char edit_buf[16];

static struct mtk *m;
static struct mtk_window *win;
static int ptr_x, ptr_y, ptr_old_x;

static int W = 320, H, min_w, min_h;
static int activated;           /* the active window: the active frame */
static struct mtk_theme theme;
static struct palette pal;
static cairo_t *cr;             /* what fill, bevel and text draw on */
static PangoLayout *plo;
static int font_asc, font_desc;
static int barh;
static struct rect r_btn, r_scale, r_pct, r_status;

static void redraw(void)
{
	if (win)
		mtk_window_redraw(win);
}

static void theme_init(void)
{
	mtk_theme_load(&theme);
	palette(&pal, &theme);
	PangoFontDescription *desc = mtk_dock_font("pulseControl");
	plo = mtk_layout_new(desc);
	/* Left to right as a paragraph, as in deskControl: text() places the text at
	 * the left of its box, and a right-to-left sink name would otherwise be flushed
	 * to its right. */
	pango_layout_set_auto_dir(plo, FALSE);
	PangoFontMetrics *fm = pango_context_get_metrics(pango_layout_get_context(plo),
		desc, NULL);
	font_asc = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(fm));
	font_desc = PANGO_PIXELS_CEIL(pango_font_metrics_get_descent(fm));
	pango_font_metrics_unref(fm);
	pango_font_description_free(desc);
	barh = font_asc + font_desc + 10;
	min_h = H = 2 * (FRAME + PAD) + 2 * barh + 4;
}

/* ---- PulseAudio --------------------------------------------------------- */

static void sink_info(pa_context *c, const pa_sink_info *i, int eol, void *u)
{
	if (eol > 0 || pending || dragging)
		return;
	redraw();
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
	/* Going, the window is about to go too: an answer would only keep quit()
	 * waiting, as long as another program goes on changing the sink. */
	if (!ctx || pa_context_get_state(ctx) != PA_CONTEXT_READY || quitting)
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
	redraw();
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
	redraw();
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
	redraw();
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
	retry_at = mtk_now() + RETRY_MS;
	redraw();
}

/* ---- drawing ------------------------------------------------------------ */

static void set_rgb(uint32_t c)
{
	mtk_set_rgb(cr, c);
}

static void fill(uint32_t px, int x, int y, int w, int h)
{
	mtk_fill(cr, px, x, y, w, h);
}

static void bevel(struct rect r, int raised)
{
	mtk_bevel(cr, r.x, r.y, r.w, r.h, BEV, raised ? pal.top : pal.bot,
		raised ? pal.bot : pal.top);
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
	/* Room for a muted volume, in brackets. */
	int pw = textw("(100 %)");
	/* Only something unusual is wider, such as a volume of 1500 %. */
	if (sink_state == SINK_OK) {
		snprintf(s, sizeof s, "(%d %%)", sink_pct);
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

static void paint(struct mtk_window *w, cairo_t *c, int width, int height, void *data)
{
	char s[32];
	int ok = sink_state == SINK_OK;
	cr = c;
	W = width;
	H = height;
	/* Glyphs hinted and placed for the buffer's pixels, not the surface's. */
	pango_cairo_update_layout(cr, plo);
	layout();
	fill(pal.bg, 0, 0, W, H);
	mtk_border(cr, W, H, activated ? &theme.active : &theme.inactive);

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
		/* Centred as the volume is when closed, so opening it moves nothing. */
		int tw = textw(s), tpos = fx + (fw - tw) / 2;
		if (edit_fresh) {
			fill(pal.sel, tpos - 1, fy + 1, tw + 2, fh - 2);
		} else {
			fill(0x000000, tpos + textw(edit_buf), fy + 2, 1, fh - 4);
		}
		text(edit_fresh ? pal.sel_fg : 0x000000, tpos, r_pct.y, r_pct.h,
			fw, s);
	} else if (ok) {
		/* Muted in brackets, not dimmed: on a mid grey the field's text has
		 * no room left to be dimmer and still read. The volume is centred
		 * without them and the "(" put before it, so that it doesn't move
		 * when they come. */
		snprintf(s, sizeof s, "%d %%", sink_pct);
		int x = fx + (fw - textw(s)) / 2;
		if (sink_mute) {
			snprintf(s, sizeof s, "(%d %%)", sink_pct);
			x -= textw("(");
		}
		text(pal.list_fg, x, r_pct.y, r_pct.h, fw, s);
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

/* Alt, Super and Ctrl make a key mean something else: Alt+Q must not quit.
 * Shift doesn't, as + takes it on many layouts. */
static int plain_key(unsigned mods)
{
	return !(mods & (MTK_MOD_ALT | MTK_MOD_LOGO | MTK_MOD_CTRL));
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
	redraw();
}

static void edit_cancel(void)
{
	if (editing) {
		editing = 0;
		redraw();
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
	redraw();
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
		redraw();
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

static void key_action(xkb_keysym_t ks, unsigned mods)
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
	if ((mods & MTK_MOD_CTRL) && mtk_key_quits(ks, mods)) {
		quitting = 1;
		return;
	}
	if (!plain_key(mods))
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

static void key(struct mtk_window *w, const struct mtk_key *k, void *data)
{
	key_action(k->sym, k->mods);
}

/* Only the volume repeats when a key is held. */
static int key_repeats(struct mtk_window *w, const struct mtk_key *k, void *data)
{
	switch (k->sym) {
	case XKB_KEY_XF86AudioRaiseVolume:
	case XKB_KEY_XF86AudioLowerVolume:
		return 1;
	case XKB_KEY_Up: case XKB_KEY_KP_Up:
	case XKB_KEY_Right: case XKB_KEY_KP_Right:
	case XKB_KEY_plus: case XKB_KEY_KP_Add:
	case XKB_KEY_Down: case XKB_KEY_KP_Down:
	case XKB_KEY_Left: case XKB_KEY_KP_Left:
	case XKB_KEY_minus: case XKB_KEY_KP_Subtract:
		return plain_key(k->mods) && !editing;
	}
	return 0;
}

/* The keyboard gone: no typing goes on. */
static void focus(struct mtk_window *w, int in, void *data)
{
	if (!in)
		edit_cancel();
}

/* Not every compositor keeps the pointer on a window while a button is held,
 * and then the release never reaches us: a drag ends at the window's edge. */
static void ptr_leave(void)
{
	end_drag();
	if (btn_pressed) {
		btn_pressed = 0;
		redraw();
	}
}

static void ptr_motion(void)
{
	int was_down = btn_down();
	/* Only sideways: a hand's jitter must not change a grabbed slider. */
	if (dragging && ptr_x != ptr_old_x)
		scale_to(ptr_x);
	if (btn_down() != was_down)
		redraw();
}

static void ptr_button(uint32_t code, int pressed)
{
	if (code != BTN_LEFT)
		return;
	if (!pressed) {
		end_drag();
		/* A click counts on the release, over the button it went down on. */
		if (btn_pressed) {
			int take = btn_down();
			btn_pressed = 0;
			redraw();
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
		redraw();
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

/* Wheels and touchpads in steps, as mtk makes them. Up means negative, and
 * louder. */
static void pointer(struct mtk_window *w, const struct mtk_pointer *p, void *data)
{
	int was_x = ptr_x;
	ptr_x = (int)p->x;
	ptr_y = (int)p->y;
	switch (p->kind) {
	case MTK_ENTER:
		break;
	case MTK_LEAVE:
		ptr_leave();
		break;
	case MTK_MOTION:
		ptr_old_x = was_x;
		ptr_motion();
		break;
	case MTK_PRESS:
	case MTK_RELEASE:
		ptr_button(p->button, p->kind == MTK_PRESS);
		break;
	case MTK_SCROLL:
		if (p->steps_y && !dragging) {
			edit_cancel();
			step_volume(-p->steps_y * STEP);
		}
		break;
	}
}

/* By the state, not the keyboard: mwc keeps a window active while a layer surface
 * such as a launcher has the keys, and draws its frames so. */
static void configure(struct mtk_window *w, int width, int height, unsigned states,
	void *data)
{
	activated = !!(states & MTK_STATE_ACTIVATED);
}

static void close_window(struct mtk_window *w, void *data)
{
	quitting = 1;
}

static const struct mtk_window_listener listener = {
	.paint = paint,
	.configure = configure,
	.close = close_window,
	.focus = focus,
	.key = key,
	.key_repeats = key_repeats,
	.pointer = pointer,
};

/* ---- main loop ---------------------------------------------------------- */

static struct pollfd wl_pfd;    /* mtk's, for this round */
static short wl_revents;

/* Pulse waits here, and for the Wayland socket too. */
static int poll_both(struct pollfd *ufds, unsigned long nfds, int timeout,
	void *u)
{
	struct pollfd fds[nfds + 1];
	memcpy(fds, ufds, nfds * sizeof *fds);
	fds[nfds] = wl_pfd;
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
	uint64_t end = mtk_now() + 1000, now;
	while (ctx && pa_context_get_state(ctx) == PA_CONTEXT_READY &&
	       pa_context_is_pending(ctx) && (now = mtk_now()) < end) {
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

	if (!(m = mtk_open(&(struct mtk_config){ .name = "pulseControl" })))
		return 1;
	theme_init();
	/* Room for all of it with the font in use, and a pixel per percent. */
	min_w = MAX(220, 2 * (FRAME + PAD) + barh + textw("(100 %)") + 2 * BEV + FIELD_PAD +
		12 + 2 * BEV + THUMB + 100);
	W = MAX(W, min_w);
	win = mtk_window_new(m, &(struct mtk_window_config){
		.width = W, .height = H, .min_width = min_w, .min_height = min_h,
	}, &listener, NULL);

	if (!(ml = pa_mainloop_new())) {
		fprintf(stderr, "pulseControl: cannot set up the PulseAudio main loop\n");
		return 1;
	}
	pa_mainloop_set_poll_func(ml, poll_both, NULL);
	ctx_start();

	for (;;) {
		if (quitting)
			quit();
		uint64_t now = mtk_now();
		if (!ctx && now >= retry_at)
			ctx_start();
		/* Also right after a start that failed at once: the wait below
		 * needs to see no context, to wake for the next attempt. */
		if (ctx_failed)
			ctx_drop();
		int timeout = -1;
		if (!ctx)
			timeout = retry_at > now ? (int)MIN(retry_at - now, (uint64_t)INT_MAX) : 0;
		mtk_before_poll(m, &wl_pfd, &timeout);
		/* A key may have asked to go: not to wait for Pulse first. */
		if (quitting) {
			mtk_after_poll(m, 0);
			continue;
		}
		wl_revents = 0; /* Pulse skips the poll when it has work queued */
		int us = timeout < 0 ? -1 : (int)MIN((int64_t)timeout * 1000, (int64_t)INT_MAX);
		if (pa_mainloop_prepare(ml, us) < 0 ||
		    (pa_mainloop_poll(ml) < 0 && errno != EINTR)) {
			mtk_after_poll(m, 0);
			fprintf(stderr, "pulseControl: the PulseAudio main loop failed\n");
			return 1;
		}
		mtk_after_poll(m, wl_revents);
		if (pa_mainloop_dispatch(ml) < 0) {
			fprintf(stderr, "pulseControl: the PulseAudio main loop failed\n");
			return 1;
		}
	}
}
