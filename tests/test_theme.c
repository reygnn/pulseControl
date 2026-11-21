/* test_theme.c - pulseControl's palette on mtk's theme: make check */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>

#include "theme.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct mtk_theme theme_of(uint32_t bg, uint32_t sel)
{
	struct mtk_theme t = { .inactive = mtk_shade(bg, mtk_text_color(bg)),
		.active = mtk_shade(sel, mtk_text_color(sel)) };
	return t;
}

/* The text on mwc's faces as Motif picks it, as mwc's titles; what is made of it
 * reads at least as well, 4.5:1 or the text's own where that is less. The volume
 * field is pulseControl's own face and reads at 4.5:1 (review 2026-11-20, F1 and
 * F2). Any grey, some hues, as background and as active colour. */
static void check_palette(uint32_t bg)
{
	struct mtk_theme t = theme_of(bg, bg);
	struct palette p;
	palette(&p, &t);
	float need = mtk_contrast_ratio(p.bg, p.fg);
	need = need < 4.5f ? need : 4.5f;
	const char *bad = p.fg != mtk_text_color(bg) ? "text" :
		p.sel_fg != mtk_text_color(bg) ? "selection" :
		mtk_contrast_ratio(p.bg, p.dim) < need ? "dimmed text" :
		mtk_contrast_ratio(p.bg, p.err) < need ? "error" :
		mtk_contrast_ratio(p.list, p.list_fg) < 4.5f ? "volume" : NULL;
	checks++;
	if (bad) {
		failures++;
		fprintf(stderr, "FAIL: bg %06x: %s\n", (unsigned)bg, bad);
	}
}

static void test_palette(void)
{
	for (uint32_t g = 0; g < 256; g++)
		check_palette(g * 0x010101);
	static const uint32_t hues[] = { 0xaeb2c3, 0x303845, 0x7387ad, 0x8b0000,
		0x2e8b57, 0xffd700, 0x4b0082, 0x008080 };
	for (size_t i = 0; i < sizeof hues / sizeof hues[0]; i++)
		check_palette(hues[i]);

	/* mwc's default look, IRIX Base: Motif's shadows, black on the light stone,
	 * and black on the built-in grey, as mtk_theme_load() gives it */
	struct mtk_theme t = { .inactive = mtk_shade(0xaaaaaa, 0),
		.active = mtk_shade(0xc6c1aa, mtk_text_color(0xc6c1aa)) };
	struct palette p;
	palette(&p, &t);
	CHECK(p.top == 0xdadada && p.bot == 0x5a5a5a);
	CHECK(p.fg == 0x000000 && p.list_fg == 0x000000 && p.err == 0x830303);
	CHECK(p.sel_fg == 0x000000 && p.paper == 0xf6f6f6);
	CHECK(p.list == 0xc4c4c4 && p.trough == 0x969696);
	/* Motif's old look: white on its focused blue */
	t = theme_of(0xaeb2c3, 0x7387ad);
	palette(&p, &t);
	CHECK(p.sel_fg == 0xffffff && p.err == 0x8a0303);
	/* the mwmrc's text colours win */
	t.inactive.fg = 0x112233;
	t.active.fg = 0xffeedd;
	palette(&p, &t);
	CHECK(p.fg == 0x112233 && p.list_fg == 0x112233 && p.sel_fg == 0xffeedd);
}

int main(void)
{
	test_palette();
	printf("test_theme: %d/%d\n", checks - failures, checks);
	return failures != 0;
}
