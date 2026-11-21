/* theme.c - the colours pulseControl draws with; see theme.h. */
#include "theme.h"

void palette(struct palette *p, const struct mtk_theme *t)
{
	uint32_t bg = t->inactive.bg;
	p->bg = bg;
	p->top = t->inactive.top;
	p->bot = t->inactive.bot;
	/* Lighter than mwc's scrollbar trough (0.25), as in osDisplay: that one is as
	 * bright as the fill beside it, which then stands out by hue alone. */
	p->trough = mtk_mix(bg, 0x000000, 0.12f);
	p->list = mtk_mix(bg, 0xffffff, 0.3f);
	p->paper = 0xf6f6f6;
	p->sel = t->active.bg;
	/* Text as mwc's titles: the resource if set, else as Motif picks it. */
	p->fg = t->inactive.fg;
	p->sel_fg = t->active.fg;
	/* mtk's, as for all mwc's clients: the error a red at 4.5:1 on any bg. */
	p->dim = t->dim;
	p->err = t->err;
	/* The field is pulseControl's own face, lighter than bg: its text reads at
	 * 4.5:1, moved from fg towards black or white as far as that needs. */
	p->list_fg = mtk_readable(p->list, p->fg, mtk_best_contrast(p->list));
}
