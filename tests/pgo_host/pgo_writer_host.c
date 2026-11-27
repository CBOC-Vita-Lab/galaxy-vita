/* Host test of the instrumented build's profile writer (vita/pgo/): a small program instrumented with -fprofile-generate whose
 * counters are written by the SAME sources the Vita build links (the vendored compiler-rt writer, vita/pgo/vita_pgo_core.c and
 * vita_pgo_platform.c on the linker's section symbols) into a raw profile that llvm-profdata must accept (run_test.sh).
 *   pgo_writer_host <out.profraw> */
#include "vita_pgo_core.h"

#ifdef PGO_I386_FREESTANDING /* run_test32.sh: an ILP32 target without a C library (i386_libc.c) */
long pgo_sys_open(const char *path);
long pgo_sys_write(int fd, const void *p, unsigned n);
long pgo_sys_close(int fd);
static int FileSink(void *ctx, const void *data, size_t size) { return pgo_sys_write((int)(long)ctx, data, (unsigned)size) != (long)size; }
#define Report(...) ((void)0)
#else
#include <stdio.h>
#include <stdlib.h>
static int FileSink(void *ctx, const void *data, size_t size) { return fwrite(data, 1, size, (FILE *)ctx) != size; }
#define Report(...) fprintf(stderr, __VA_ARGS__)
#endif

typedef int (*Op)(int);
static int Twice(int x) { return x * 2; }
static int Square(int x) { return x * x; }
static int Neg(int x) { return -x; }
/* (noinline + volatile: the loops must stay loops, the indirect call stays indirect, so that the counts are predictable) */
__attribute__((noinline)) int Collatz(int n) {
  int steps = 0;
  while (n != 1) {
    n = (n & 1) ? 3 * n + 1 : n / 2;
    ++steps;
  }
  return steps;
}
__attribute__((noinline)) int Apply(Op op, int x) { return op(x); }
__attribute__((noinline)) void Fill(char *dst, const char *src, unsigned n) { __builtin_memcpy(dst, src, n); }

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  Op volatile ops[3] = {Twice, Square, Neg};
  long sum = 0;
  for (int i = 1; i <= 1000; ++i) sum += Collatz(i);               /* Collatz: 1000 calls, 59542 loop trips in all */
  for (int i = 0; i < 300; ++i) sum += Apply(ops[i % 3 == 2 ? 0 : (i % 3)], i);  /* indirect calls: Twice 200, Square 100, Neg 0 */
  char a[64], b[64] = "profile";
  for (int i = 0; i < 50; ++i) Fill(a, b, 8);                       /* memcpy size 8, 50 times (value-profiled) */
  Report("functions %llu counters %llu size %llu sum %ld\n", (unsigned long long)vita_pgo_num_functions(),
         (unsigned long long)vita_pgo_num_counters(), (unsigned long long)vita_pgo_size(), sum + a[0]);
#ifdef PGO_I386_FREESTANDING
  const long fd = pgo_sys_open(argv[1]);
  if (fd < 0 || vita_pgo_write(FileSink, (void *)fd) || pgo_sys_close((int)fd)) return 1;
#else
  FILE *f = fopen(argv[1], "wb");
  if (!f || vita_pgo_write(FileSink, f) || fclose(f)) return 1;
#endif
  return 0;
}
