# Caveats

Limits of pulseControl that are known and left as they are.


## Sound

It controls the default output only: no other outputs, no inputs, no single programs.

The scale ends at 100 %. Above that a volume can only come from another program, and
pulseControl only lowers it.

A typed volume above 0 ends the muting; a typed 0 and every other change leave it.


## Size

The window can be resized only from the keyboard (Alt+F8 in mwc), since it has no
resize handles. The scale grows with the width.

It draws at the integer scale the compositor asks for with wl_compositor 6. A
fractional scale is drawn at the next integer and scaled down by the compositor; an
older compositor never asks, and there the window stays at scale 1 and is blurred.
