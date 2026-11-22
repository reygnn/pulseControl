/* rc.c - resources from mwc's mwmrc, and the theme from them. */
#define _GNU_SOURCE
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mtk.h"
#include "mwc_color.h"

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

/* As mwc's add_rule(): "Mwm*Client*resource", '.' and '*' both separate, the
 * client is all between the first and the last part and may have dots of its own.
 * app NULL: only global lines. */
static int key_matches(const char *k, const char *app, const char *res)
{
	size_t prefix = strcspn(k, "*.");
	const char *r = k + strlen(k);
	while (r > k && r[-1] != '*' && r[-1] != '.')
		r--;
	if (prefix != 3 || !k[prefix] || strncasecmp(k, "mwm", 3) || !*r ||
	    strcasecmp(r, res))
		return 0;
	if (r - k == (ptrdiff_t)prefix + 1)
		return !app;
	size_t len = r - k - prefix - 2;
	const char *c = k + prefix + 1;
	return app && len > 0 && !memchr(c, '*', len) && strlen(app) == len &&
		!strncasecmp(c, app, len);
}

static char *lookup(const char *app, const char *key, char *val, size_t n)
{
	char path[PATH_MAX];
	const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
	int len;
	if (xdg && *xdg)
		len = snprintf(path, sizeof path, "%s/mwc/mwmrc", xdg);
	else if (home)          /* as mwc: an empty HOME is /.config */
		len = snprintf(path, sizeof path, "%s/.config/mwc/mwmrc", home);
	else
		return NULL;    /* as mwc: no HOME, no mwmrc */
	if (len < 0 || (size_t)len >= sizeof path)
		return NULL;
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char *found = NULL, *line = NULL;       /* whole lines, however long */
	size_t cap = 0;
	while (getline(&line, &cap, f) > 0) {
		char *colon = strchr(line, ':');
		if (!colon)
			continue;
		*colon = 0;
		if (!key_matches(trim(line), app, key))
			continue;
		char *v = trim(colon + 1);
		/* The last line wins, as in mwc: one too long to keep is no value,
		 * not a cut one and not the line before it. */
		found = NULL;
		size_t vl = strlen(v);
		if (vl < n) {
			memcpy(val, v, vl + 1);
			found = val;
		}
	}
	free(line);
	fclose(f);
	return found;
}

char *mtk_mwmrc_get(const char *key, char *val, size_t n)
{
	return lookup(NULL, key, val, n);
}

char *mtk_mwmrc_client_get(const char *app, const char *key, char *val, size_t n)
{
	return app && *app ? lookup(app, key, val, n) : NULL;
}

static int rc_color(const char *res, uint32_t *out)
{
	char v[256];
	return mtk_parse_color(mtk_mwmrc_get(res, v, sizeof v), out);
}

/* As mwc's setting_color(): the environment, the mwmrc, the default. One that is no
 * colour counts as not set. */
static uint32_t setting(const char *env, const char *res, uint32_t def)
{
	uint32_t rgb;
	if (mtk_parse_color(getenv(env), &rgb) || rc_color(res, &rgb))
		return rgb;
	return def;
}

/* A list and the row selected on it, a field, urgent, a trough and a folder, faces of the
 * clients' own, and the dimmed
 * and the error text on the window. The list is lighter
 * than the window; the active colour, as mwc's titles have it, often lands on its
 * brightness (IRIX's Base: #c4c4c4 and #c6c1aa, 1.04:1), so the selection is moved
 * apart. Text that was chosen, textForeground or a client's own, stays as far as it
 * reads; Motif's own is left for the better of black and white. */
void mtk_theme_lists(struct mtk_theme *t)
{
	uint32_t bg = t->inactive.bg, fg = t->inactive.fg;
	uint32_t list = mtk_mix(bg, 0xffffff, 0.3f), lbest = mtk_best_contrast(list);
	int chosen = t->has_fg || fg != mtk_text_color(bg);
	t->list = mtk_shade(list, chosen ? mtk_readable(list, fg, lbest) : lbest);
	uint32_t field = 0xf6f6f6, fbest = mtk_best_contrast(field);
	t->field = mtk_shade(field, chosen ? mtk_readable(field, fg, fbest) : fbest);
	uint32_t urgent = mtk_mix(bg, 0xffa000, 0.7f);
	t->urgent = mtk_shade(urgent, mtk_best_contrast(urgent));
	t->trough = mtk_mix(bg, 0x000000, 0.12f);
	t->folder = 0xc9a85c;
	uint32_t sel = mtk_apart(list, t->active.bg, MTK_SELECT_CONTRAST);
	t->select = mtk_shade(sel, mtk_readable(sel, t->active.fg, mtk_best_contrast(sel)));
	t->dim = mtk_dimmed(bg, fg);
	/* Towards the better of black and white, not fg: Motif's white on a mid grey
	 * has less than 4.5:1 itself, and a red moved towards it never gets there. */
	uint32_t best = mtk_best_contrast(bg);
	t->err = mtk_readable(bg, best == 0xffffff ? 0xffa0a0 : 0x8a0303, best);
}

void mtk_theme_load(struct mtk_theme *t)
{
	/* The client palette, as IRIX's programs drew: basicBackground with
	 * textForeground, else mwc's #c4c4c4 with black; the frame colours are mwc's
	 * alone, but for the active one, a client's border and selection. The text is
	 * small, so it gets WCAG's 4.5:1 where the palette or Motif's rule falls short. */
	uint32_t act = setting("MWC_ACTIVE", "activeBackground", COLOR_ACTIVE);
	uint32_t basic = COLOR_BASIC, text = COLOR_TEXT, afg;
	int has_basic = rc_color("basicBackground", &basic);
	t->has_fg = rc_color("textForeground", &text);
	t->has_active_fg = rc_color("activeForeground", &afg);
	/* A face that is set and no text: the better of black and white. */
	uint32_t best = mtk_best_contrast(basic), abest = mtk_best_contrast(act);
	if (has_basic && !t->has_fg)
		text = best;
	t->inactive = mtk_shade(basic, mtk_readable(basic, text, best));
	t->active = mtk_shade(act, mtk_readable(act, t->has_active_fg ? afg : abest, abest));
	mtk_theme_lists(t);
}
