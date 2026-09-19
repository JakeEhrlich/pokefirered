// Wrapper around the game's global.h.
// On Apple hosts the real global.h defines _(x) as (x) ("IDE support"), which would stop tools/preproc
// from charmap-encoding string literals. Restore self-referential macros so preproc still sees _("...").
#ifndef GUARD_SIM_GLOBAL_WRAPPER_H
#define GUARD_SIM_GLOBAL_WRAPPER_H
#include_next "global.h"
#undef _
#define _(x) _(x)
#undef __
#define __(x) __(x)
#endif
