/* The raw-profile writer of the instrumented build (vita_pgo_core.c). */
#ifndef VITA_PGO_CORE_H
#define VITA_PGO_CORE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Receives the next `size` bytes of the profile in order; nonzero = failure (the write stops and vita_pgo_write returns -1). */
typedef int (*vita_pgo_sink)(void *ctx, const void *data, size_t size);
/* A snapshot of the counters now (they keep running; a counter that changes under the writer is read torn at worst by one increment). */
int vita_pgo_write(vita_pgo_sink sink, void *ctx);
uint64_t vita_pgo_size(void);           /* the size of the whole raw profile in bytes */
uint64_t vita_pgo_num_functions(void);  /* the records in __llvm_prf_data */
uint64_t vita_pgo_num_counters(void);
#ifdef __cplusplus
}
#endif
#endif
