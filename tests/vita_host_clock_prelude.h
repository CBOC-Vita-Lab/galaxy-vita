// glibc/libstdc++ test compatibility only, never part of the native Vita build.
#pragma once
#include <cmath>
#include <string>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#if defined(__GNUC__) && !defined(__clang__)
// glibc already declares these private names; the Wii intrinsic shims are unused here.
#define __fabsf host_clock_unused_fabsf
#define __fabs host_clock_unused_fabs
#define _Static_assert static_assert
#endif
#include "port/compat.h"
