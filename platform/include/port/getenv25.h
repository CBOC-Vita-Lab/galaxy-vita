// getenv25 (25 Nov 2026): switches that were read from the environment on every pass, upload or fresh draw, now read once.
// The console's newlib getenv walks the whole environment (several hundred names after entry.cpp's defaults) every call, and takes the
// environment lock; a sister project measured it on a console. Each value below is set by env.txt / entry.cpp's setenv defaults before the
// first read and by nothing after: none is in a stage preset's table (port/stage_knob.h), the Settings menu's key tables or the quality sets.
// Same parsing as vgx::EnvInt (an empty value counts as not set; strtol, base 0), same defaults; errno is left as the caller had it.
// PORT_HOT_KNOB (port/hot_knob.h): one relaxed load a call, no guard and no barrier.
#pragma once

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>

#include "port/hot_knob.h"

namespace port {
namespace getenv25 {

inline long Read(const char* name, long dflt) {
    const int savedErrno = errno;
    const char* v = std::getenv(name);
    const long r = (v && *v) ? std::strtol(v, nullptr, 0) : dflt;
    errno = savedErrno;
    return r;
}
// GXV_PASSDIRECT (default 1): a pass's colour buffer may be the copy's own texture. Asked per pass (PassTargetOf, LateDirect).
inline bool PassDirectOn() { return PORT_HOT_KNOB(bool, Read("GXV_PASSDIRECT", 1) != 0); }
// GXV_TEXUP_TINY_BYTES (default 0): at most this many bytes, one level: the upload goes the linear way. Asked per swizzled upload.
inline int TexupTinyBytes() {
    return PORT_HOT_KNOB(int, [] {
        const long v = Read("GXV_TEXUP_TINY_BYTES", 0);
        return static_cast<int>(v < 0 ? 0L : (v > static_cast<long>(INT_MAX) ? static_cast<long>(INT_MAX) : v));
    }());
}
// GXV_HRW_POINTER (default 0): the star pointer in the fresh second picture. Asked per fresh draw (a picture of the 60 Assist pair).
inline int HrwPointer() { return PORT_HOT_KNOB(int, static_cast<int>(Read("GXV_HRW_POINTER", 0))); }
// GXV_OVERSCAN / GXV_PREVCAP (default 0): asked per marked capture of a picture (MarkTransientCopies, with GXV_INDCAP_VIS on).
inline bool OverscanOn() { return PORT_HOT_KNOB(bool, Read("GXV_OVERSCAN", 0) != 0); }
inline bool PrevCapOn() { return PORT_HOT_KNOB(bool, Read("GXV_PREVCAP", 0) != 0); }

}  // namespace getenv25
}  // namespace port
