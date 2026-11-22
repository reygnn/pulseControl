/*
 * color.h: the colours mwc and its clients share, on 0xrrggbb: the built-in defaults
 * and Motif 2.3.8's title text and shadows. No dependencies beyond the C library, so
 * mtk takes color.c and color.h unchanged; mwc is where they are changed.
 */
#ifndef MWC_COLOR_H
#define MWC_COLOR_H

#include <stdint.h>

/* mwc's own: a blue focused frame, grey for the rest, and IRIX's client palette, the
 * surface of mtk's clients. Motif gives the blue white text and the greys black. */
#define COLOR_ACTIVE 0x2e5a8cu
#define COLOR_INACTIVE 0xb4b4b4u
#define COLOR_BASIC 0xc4c4c4u
#define COLOR_TEXT 0x000000u
#define COLOR_ROOT 0x5b7792u

struct color_shades {
	uint32_t top, bottom;
};

/* Motif's title text on bg: black above 70% of its brightness, white otherwise. */
uint32_t color_text(uint32_t bg);

/* The top and bottom shadows of bg as Motif computes them, cut to 8 bits as a TrueColor
 * screen shows them. */
struct color_shades color_shades(uint32_t bg);

#endif
