/* util.h - small pieces of sc.c without Wayland: the buffer's scale and the seats
 * the compositor has announced. test_util checks them. The seats are the same as
 * digitalClock's. */
#ifndef UTIL_H
#define UTIL_H

/* The scale to draw a w x h window at, asked for sc: lower while the buffer would
 * not fit, its pool's size being an int32 and cairo's image side at most 32767
 * pixels. At least 1. */
int draw_scale(int w, int h, int sc);

/* The seats the compositor has announced and not removed, oldest first, by their
 * name in the registry and version: one is bound, the others wait for it to go. */
#define MAX_SEATS 8
struct seats {
	unsigned name[MAX_SEATS], ver[MAX_SEATS];
	int n;
};

/* A seat announced; 0 if the list is full and it is not kept. */
int seats_add(struct seats *s, unsigned name, unsigned ver);

/* A global removed: dropped if it is a seat in the list. */
void seats_remove(struct seats *s, unsigned name);

/* The seat to take, the oldest left; 0 if there is none. */
int seats_next(const struct seats *s, unsigned *name, unsigned *ver);

#endif
