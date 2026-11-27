// Galaxy 3.0 multiplayer (beta diagnostics): what mp_diag.cpp and mp_netcheck.cpp share. Not for the game side (port/mp_diag.h is).
#ifndef MP_DIAG_INT_H
#define MP_DIAG_INT_H

#include "port/mp_diag.h"

// The check finished: it becomes the "check" of the next problem report (and is kept in the small state file).
void mp_diag_store_check(const MpDiagCheck* c);

#endif
