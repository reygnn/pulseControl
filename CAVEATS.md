# Caveats

Limits of pulseControl that are known and left as they are.


## Sound

It controls the default output only: no other outputs, no inputs, no single programs.

The scale ends at 100 %. Above that a volume can only come from another program, and
pulseControl only lowers it.


## Size

The window can be resized only from the keyboard (Alt+F8 in mwc), since it has no
resize handles. The scale grows with the width.

There is no output scale: on a scaled output the window is scaled up and blurred.
