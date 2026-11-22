/* test_theme.c - pulseControl's palette on mtk's theme: make check */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>

#include "theme.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* A theme as mtk_theme_load() makes it of a face and an active colour set without
 * their text: the better of black and white on each. */
static struct mtk_theme theme_of(uint32_t bg, uint32_t sel)
{
	struct mtk_theme t = { .inactive = mtk_shade(bg, mtk_best_contrast(bg)),
		.active = mtk_shade(sel, mtk_best_contrast(sel)) };
	mtk_theme_lists(&t);
	return t;
}

/* Every text reads at 4.5:1 on its face: on the window, dimmed, as an error, on the
 * selection, in the volume field and typed (review 2026-11-20, F1 and F2). Any grey,
 * some hues, as background and as active colour. */
static void check_palette(uint32_t bg)
{
	struct mtk_theme t = theme_of(bg, bg);
	struct palette p;
	palette(&p, &t);
	const char *bad = mtk_contrast_ratio(p.bg, p.fg) < 4.5f ? "text" :
		mtk_contrast_ratio(p.sel, p.sel_fg) < 4.5f ? "selection" :
		mtk_contrast_ratio(p.bg, p.dim) < 4.5f ? "dimmed text" :
		mtk_contrast_ratio(p.bg, p.err) < 4.5f ? "error" :
		mtk_contrast_ratio(p.list, p.list_fg) < 4.5f ? "volume" :
		mtk_contrast_ratio(p.field, p.field_fg) < 4.5f ? "typed volume" : NULL;
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

	/* mtk's defaults: black on #c4c4c4 with Motif's shadows, white on #2e5a8c */
	struct mtk_theme t = theme_of(0xc4c4c4, 0x2e5a8c);
	struct palette p;
	palette(&p, &t);
	CHECK(p.bg == 0xc4c4c4 && p.top == 0xe6e6e6 && p.bot == 0x6c6c6c);
	CHECK(p.fg == 0x000000 && p.dim == 0x4e4e4e && p.err == 0x8a0303);
	CHECK(p.sel == 0x2e5a8c && p.sel_fg == 0xffffff && p.trough == 0xacacac);
	CHECK(p.list == 0xd6d6d6 && p.list_fg == 0x000000);
	CHECK(p.field == 0xf6f6f6 && p.field_fg == 0x000000);
}

/* Every colour is the theme's: pulseControl sets none of its own. Each face of t
 * set to one mtk would never give shows that palette() takes it from there. */
static void test_from_theme(void)
{
	struct mtk_theme t = theme_of(0xc4c4c4, 0x2e5a8c);
	t.inactive = (struct mtk_shade){ .bg = 0x010101, .fg = 0x020202, .top = 0x030303,
		.bot = 0x040404 };
	t.active = (struct mtk_shade){ .bg = 0x050505, .fg = 0x060606 };
	t.list = (struct mtk_shade){ .bg = 0x070707, .fg = 0x080808 };
	t.field = (struct mtk_shade){ .bg = 0x090909, .fg = 0x0a0a0a };
	t.trough = 0x0b0b0b;
	t.dim = 0x0c0c0c;
	t.err = 0x0d0d0d;
	struct palette p;
	palette(&p, &t);
	CHECK(p.bg == 0x010101 && p.fg == 0x020202 && p.top == 0x030303 &&
		p.bot == 0x040404);
	CHECK(p.sel == 0x050505 && p.sel_fg == 0x060606);
	CHECK(p.trough == 0x0b0b0b);
	CHECK(p.list == 0x070707);
	CHECK(p.list_fg == 0x080808);
	CHECK(p.field == 0x090909);
	CHECK(p.field_fg == 0x0a0a0a);
	CHECK(p.dim == 0x0c0c0c && p.err == 0x0d0d0d);
}

int main(void)
{
	test_palette();
	test_from_theme();
	printf("test_theme: %d/%d\n", checks - failures, checks);
	return failures != 0;
}
