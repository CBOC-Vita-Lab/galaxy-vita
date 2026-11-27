/* Used by run_synth.sh: a 32-bit program that carries the profile sections of REAL instrumented Vita objects (data records, counters,
 * names, cut out of the linked ELF with objcopy; i386_synth.S puts them back into sections of the same names, in the same order and
 * adjacency) and writes them with the Vita build's writer sources.  The counters are filled with made-up values: the point is that the
 * records the compiler made for the real game and renderer units, written by this runtime in the raw format of an ILP32 little-endian
 * target, are accepted by llvm-profdata and apply to the same units under -fprofile-use.  (No value-profile data: the records' value
 * pointers are ELF addresses of the Vita.) */
#include <stddef.h>
#include <stdint.h>

#include "InstrProfiling.h"
#include "InstrProfilingInternal.h"

long pgo_sys_open(const char *path);
long pgo_sys_write(int fd, const void *p, unsigned n);
long pgo_sys_close(int fd);

static int Sink(void *ctx, const void *data, size_t size) { return pgo_sys_write((int)(long)ctx, data, (unsigned)size) != (long)size; }
typedef struct { int (*sink)(void *, const void *, size_t); void *ctx; } S;
static uint32_t W(ProfDataWriter *w, ProfDataIOVec *v, uint32_t n) {
  S *s = (S *)w->WriterCtx;
  static const unsigned char z[256];
  for (uint32_t i = 0; i < n; ++i) {
    size_t t = v[i].ElmSize * v[i].NumElm;
    if (v[i].Data) { if (t && s->sink(s->ctx, v[i].Data, t)) return 1; }
    else while (t) { size_t k = t < sizeof z ? t : sizeof z; if (s->sink(s->ctx, z, k)) return 1; t -= k; }
  }
  return 0;
}
int main(int argc, char **argv) {
  if (argc < 3) return 2;
  const unsigned long mode = (unsigned long)(argv[2][0] - '0'); /* counters: 0 zero, 1 all ones, 2 a spread */
  char *c = __llvm_profile_begin_counters();
  const uint64_t n = (uint64_t)(__llvm_profile_end_counters() - c) / 8;
  for (uint64_t i = 0; i < n; ++i) ((uint64_t *)c)[i] = mode == 0 ? 0 : mode == 1 ? 1 : 1 + ((uint32_t)i * 2654435761u) % 1000u;
  const long fd = pgo_sys_open(argv[1]);
  if (fd < 0) return 1;
  S s = {Sink, (void *)fd};
  ProfDataWriter w = {W, &s};
  const int rc = lprofWriteData(&w, 0, 0);
  pgo_sys_close((int)fd);
  return rc ? 1 : 0;
}
