/* theme.h - the colours pulseControl draws with, from mtk's theme, without Wayland.
 * test_theme checks them. */
#ifndef THEME_H
#define THEME_H

#include <stdint.h>

#include "mtk.h"

/* Each text colour for the face it is on. */
struct palette {
	uint32_t bg, top, bot;          /* the face and its bevels, as mwc's */
	uint32_t trough, sel;           /* the scale: empty and filled */
	uint32_t list, paper;           /* the volume field, and while typing */
	uint32_t fg, dim, err;          /* text on bg: normal, dimmed, error */
	uint32_t list_fg;               /* text on list */
	uint32_t sel_fg;                /* text on sel */
};

/* The palette for mwc's colours: bg and its text as t's inactive face, the fill and
 * its text as t's active one. */
void palette(struct palette *p, const struct mtk_theme *t);

#endif
