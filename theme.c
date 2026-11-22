/* theme.c - the colours pulseControl draws with; see theme.h. */
#include "theme.h"

void palette(struct palette *p, const struct mtk_theme *t)
{
	p->bg = t->inactive.bg;
	p->top = t->inactive.top;
	p->bot = t->inactive.bot;
	p->trough = t->trough;
	p->sel = t->active.bg;
	p->list = t->list.bg;
	p->field = t->field.bg;
	p->fg = t->inactive.fg;
	p->dim = t->dim;
	p->err = t->err;
	p->list_fg = t->list.fg;
	p->field_fg = t->field.fg;
	p->sel_fg = t->active.fg;
}
