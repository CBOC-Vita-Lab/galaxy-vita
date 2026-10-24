// Metrowerks runtime helpers referenced directly by game code.
#pragma once
#include <stdint.h>
static inline uint64_t __cvt_dbl_usll(double d) { return (uint64_t)d; }
static inline uint32_t __cvt_fp2unsigned(double d) { return (uint32_t)d; }
