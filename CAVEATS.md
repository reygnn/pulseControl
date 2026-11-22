# Caveats

Limits of pulseControl that are known and left as they are.


## Sound

It controls the default output only: no other outputs, no inputs, no single programs.

The scale ends at 100 %. Above that a volume can only come from another program, and
pulseControl only lowers it.

A typed volume above 0 ends the muting; a typed 0 and every other change leave it.

While the slider is held or a change is on its way, a new default output waits: the
volume goes to the old one until the release or the answer, then the window follows.


## Size

The window can be resized only from the keyboard (Alt+F8 in mwc), since it has no
resize handles. The scale grows with the width.

It starts 151 wide, digitalClock's width with its default formats and DejaVu Sans 10.
digitalClock with another `CLOCK_FORMAT` or `CLOCK_DATE` is wider or narrower, and
pulseControl doesn't follow. It is never narrower than a pixel per percent on the
scale and a muted volume below it need.

The info popup is as wide as its text when it opens; a longer one that comes while it
is up is cut. It opens upwards from the window's top, which suits the dock at the
bottom of the screen. Where there is room below, it opens downwards from there and
covers the window while it is up.

It draws at the integer scale the compositor asks for with wl_compositor 6. A
fractional scale is drawn at the next integer and scaled down by the compositor; an
older compositor never asks, and there the window stays at scale 1 and is blurred.

The scale is at most 16, as in digitalClock: a compositor asking for more gets the
window at 16 and scales it up. A very large window gets a lower scale still, so that
its buffer fits an int32 pool and cairo's 32767 pixels a side.

The cursor is the theme's nearest size to `XCURSOR_SIZE` times the scale;
`XCURSOR_SIZE` counts up to 256, more than themes have. When the scale doesn't divide
that image, wp_viewporter shows it at its size over the scale. A compositor without
wp_viewporter gets it at the largest scale that divides the image, so it can come out
larger than `XCURSOR_SIZE` and less sharp.


## Input

It keeps the first eight seats the compositor announces. One beyond that is taken
only if it is announced again after the others went. The headless mwc of `make test`
has one seat, so mtk's unit test checks the list, not the change of seat.
