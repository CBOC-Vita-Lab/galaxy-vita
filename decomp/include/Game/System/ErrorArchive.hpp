#pragma once

#include <revolution.h>

// The error message archive the original main.dol holds in .rodata (the disc
// error screens' texts, font and window).  The port takes it from the
// player's own main.dol (tools/cook/cook.py) and loads it at boot into this
// writable buffer: platform/src/dvd/dol_data.cpp.
extern "C" u8 cErrorArchive[];
