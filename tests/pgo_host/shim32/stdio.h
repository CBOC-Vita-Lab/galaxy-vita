/* Just enough of a C library for the profile runtime in the 32-bit freestanding host test (run_test32.sh). */
#pragma once
#include <stddef.h>
typedef struct _FILE FILE;
extern FILE *stderr;
int fprintf(FILE *f, const char *fmt, ...);
