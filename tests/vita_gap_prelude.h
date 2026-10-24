// Linux test headers only; native Vita compiles the production source unchanged.
#pragma once
#include <cstdio>
#include <cstring>
#include "vita_host_clock_prelude.h"
// The SDK declares this without storage class; the runtime defines it static.
extern "C" { static void __OSReschedule(void); }
