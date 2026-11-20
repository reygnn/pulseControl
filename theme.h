/* theme.h - the colours of pulseControl without Wayland: mwc's rules for the
 * bevels and the text, colour names, and the mwmrc. The same as digitalClock's,
 * plus readable(). test_theme checks them. */
#ifndef THEME_H
#define THEME_H

#include <stddef.h>
#include <stdint.h>

/* rgb moved towards to by f, per channel, rounded. */
uint32_t mix(uint32_t rgb, uint32_t to, float f);

/* WCAG relative luminance of 0xrrggbb. */
float luminance(uint32_t c);

/* WCAG contrast ratio of a and b, 1 to 21. */
float contrast_ratio(uint32_t a, uint32_t b);

/* Black or white, whichever has the higher WCAG contrast on bg. */
uint32_t contrast(uint32_t bg);

/* Dimmed text: bg moved towards fg as far as WCAG AA's 4.5:1 for small text
 * needs, at least 12/20 of the way; fg itself if even that falls short. */
uint32_t dimmed(uint32_t bg, uint32_t fg);

/* c as it is if it has 4.5:1 on bg, else moved towards fg just as far as that
 * needs; fg itself if even that falls short. For a colour such as the error red,
 * which is picked for a light or a dark face but not for the greys between. */
uint32_t readable(uint32_t bg, uint32_t c, uint32_t fg);

/* The colours pulseControl draws with, each text colour for the face it is on. */
struct palette {
	uint32_t bg, top, bot;          /* the face and its bevels, as mwc's */
	uint32_t trough, sel;           /* the scale: empty and filled */
	uint32_t list, paper;           /* the volume field, and while typing */
	uint32_t fg, dim, err;          /* text on bg: normal, dimmed, error */
	uint32_t list_fg, list_dim;     /* text on list: normal, muted */
	uint32_t sel_fg;                /* text on sel */
};

/* The palette for background bg and fill sel. fg and sel_fg are the text colours
 * from the mwmrc, or NULL for black or white, whichever reads better. */
void palette(struct palette *p, uint32_t bg, uint32_t sel, const uint32_t *fg,
	const uint32_t *sel_fg);

/* What mwc reads: "#rrggbb", names like "gold", and rgb:r/g/b with 1 to 4 hex
 * digits per channel. 0 for NULL, "" and anything else; *out is then untouched. */
int parse_color(const char *s, uint32_t *out);

/* The value of the last global "Mwm*key: value" line of $XDG_CONFIG_HOME/mwc/mwmrc,
 * else ~/.config/mwc/mwmrc, in val[n]; NULL if there is none, or it is too long. */
char *mwmrc_get(const char *key, char *val, size_t n);

#endif
