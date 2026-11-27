#pragma once
#include <stddef.h>
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void free(void *p);
char *getenv(const char *name);
int atoi(const char *s);
