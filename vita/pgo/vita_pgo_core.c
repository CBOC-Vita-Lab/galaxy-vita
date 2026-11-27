/* The raw-profile writer of the instrumented build, without the Vita: the vendored compiler-rt writer (lprofWriteData, the one the
 * stock runtime's file writer calls) feeding an arbitrary sink.  vita_pgo.cpp passes sceIo; tests/pgo_writer_host.c passes stdio.
 * Written for this port (tools/pgo/README.md, "Record on the console"). */
#include <string.h>

#include "InstrProfiling.h"
#include "InstrProfilingInternal.h"
#include "vita_pgo_core.h"

typedef struct { vita_pgo_sink sink; void *ctx; int failed; } SinkCtx;

/* A ProfDataWriter callback: every IO vector is a run of ElmSize*NumElm bytes, or (Data null) zero padding that the format keeps
 * between the sections (Data null without UseZeroPadding would mean "skip", which a sequential sink cannot do: the stock file
 * writer seeks; this one is never asked to, but writes zeros for it so that the offsets stay right). */
static uint32_t SinkWriter(ProfDataWriter *This, ProfDataIOVec *IOVecs, uint32_t NumIOVecs) {
  SinkCtx *c = (SinkCtx *)This->WriterCtx;
  static const unsigned char zeros[256];
  uint32_t i;
  for (i = 0; i < NumIOVecs; ++i) {
    size_t total = IOVecs[i].ElmSize * IOVecs[i].NumElm;
    if (IOVecs[i].Data) {
      if (total && c->sink(c->ctx, IOVecs[i].Data, total)) { c->failed = 1; return 1; }
    } else {
      while (total) {
        size_t n = total < sizeof zeros ? total : sizeof zeros;
        if (c->sink(c->ctx, zeros, n)) { c->failed = 1; return 1; }
        total -= n;
      }
    }
  }
  return 0;
}

int vita_pgo_write(vita_pgo_sink sink, void *ctx) {
  SinkCtx c = {sink, ctx, 0};
  ProfDataWriter w;
  w.Write = SinkWriter;
  w.WriterCtx = &c;
  if (lprofWriteData(&w, lprofGetVPDataReader(), 0)) return -1;
  return c.failed ? -1 : 0;
}

uint64_t vita_pgo_size(void) { return __llvm_profile_get_size_for_buffer(); }
uint64_t vita_pgo_num_functions(void) { return __llvm_profile_get_num_data(__llvm_profile_begin_data(), __llvm_profile_end_data()); }
uint64_t vita_pgo_num_counters(void) { return __llvm_profile_get_num_counters(__llvm_profile_begin_counters(), __llvm_profile_end_counters()); }
