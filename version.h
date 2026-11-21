/* version.h - the one place pulseControl's version is set; pulseControl -v reports it.
 * PULSECONTROL_GIT, from the Makefile, adds the commit built: +DATE.TIME.HASH. */
#ifndef PULSECONTROL_VERSION_H
#define PULSECONTROL_VERSION_H

#include "build/gitversion.h"
#define PULSECONTROL_VERSION "0.1.0" PULSECONTROL_GIT

#endif
