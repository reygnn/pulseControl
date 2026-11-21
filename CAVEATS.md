# Caveats

Limits of pulseControl that are known and left as they are.


## Sound

It controls the default output only: no other outputs, no inputs, no single programs.

The scale ends at 100 %. Above that a volume can only come from another program, and
pulseControl only lowers it.

A typed volume above 0 ends the muting; a typed 0 and every other change leave it.

While the slider is held or a change is on its way, a new default output waits: the
volume goes to the old one until the release or the answer, then the window follows.


## Look

The scale's trough is lighter than mwc's scrollbar trough, as in osDisplay: at mwc's
shade it is as bright as the fill in the active colour beside it.


## Size

The window can be resized only from the keyboard (Alt+F8 in mwc), since it has no
resize handles. The scale grows with the width.

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


## Colours

Without `Mwm*foreground` and `Mwm*activeForeground` the text is black or white by
Motif's rule, as mwc's titles; on mwc's built-in grey black, as mwc has it. With mwc's
default colours that reads well. A mid-tone set as a colour can fall short of WCAG's
4.5:1: on Motif's old focused blue `#7387ad` the selected volume is white at about
3.6:1. Dimmed text and errors are never harder to read than the text. The volume field
is pulseControl's own face and keeps 4.5:1.


## Input

It keeps the first eight seats the compositor announces. One beyond that is taken
only if it is announced again after the others went. The headless mwc of `make test`
has one seat, so mtk's unit test checks the list, not the change of seat.
