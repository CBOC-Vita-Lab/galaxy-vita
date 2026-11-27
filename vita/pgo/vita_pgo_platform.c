/* The PS Vita platform layer of clang's profile runtime (instrumented "profile" builds only: VITA_PGO_GENERATE=1, tools/pgo/README.md).
 *
 * compiler-rt has no platform for arm-vita-eabi, but the instrumented objects are plain ELF: the compiler puts the counters, the
 * per-function records, the names and the value-profile nodes into the sections __llvm_prf_cnts, __llvm_prf_data, __llvm_prf_names and
 * __llvm_prf_vnds, and GNU ld defines __start_<section> / __stop_<section> for an orphan section whose name is a C identifier and that
 * something references.  This file is InstrProfilingPlatformLinux.c with the start/stop symbols and nothing else (no build id, no dl
 * iteration, no shared objects): the same section bounds, so the raw profile the vendored writer (compiler-rt/) makes is the one the
 * Linux harness makes.  Written for this port; the code it follows is under Apache-2.0 with the LLVM exception (licenses/). */
#include "InstrProfiling.h"
#include "InstrProfilingInternal.h"

#define PROF_DATA_START INSTR_PROF_SECT_START(INSTR_PROF_DATA_COMMON)
#define PROF_DATA_STOP INSTR_PROF_SECT_STOP(INSTR_PROF_DATA_COMMON)
#define PROF_NAME_START INSTR_PROF_SECT_START(INSTR_PROF_NAME_COMMON)
#define PROF_NAME_STOP INSTR_PROF_SECT_STOP(INSTR_PROF_NAME_COMMON)
#define PROF_CNTS_START INSTR_PROF_SECT_START(INSTR_PROF_CNTS_COMMON)
#define PROF_CNTS_STOP INSTR_PROF_SECT_STOP(INSTR_PROF_CNTS_COMMON)
#define PROF_BITS_START INSTR_PROF_SECT_START(INSTR_PROF_BITS_COMMON)
#define PROF_BITS_STOP INSTR_PROF_SECT_STOP(INSTR_PROF_BITS_COMMON)
#define PROF_ORDERFILE_START INSTR_PROF_SECT_START(INSTR_PROF_ORDERFILE_COMMON)
#define PROF_VNODES_START INSTR_PROF_SECT_START(INSTR_PROF_VNODES_COMMON)
#define PROF_VNODES_STOP INSTR_PROF_SECT_STOP(INSTR_PROF_VNODES_COMMON)

/* Weak: a section no object has (no value-profile nodes, no MC/DC bitmap) leaves its pair null and the runtime treats it as empty. */
extern __llvm_profile_data PROF_DATA_START COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern __llvm_profile_data PROF_DATA_STOP COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern char PROF_CNTS_START COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern char PROF_CNTS_STOP COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern char PROF_BITS_START COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern char PROF_BITS_STOP COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern uint32_t PROF_ORDERFILE_START COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern char PROF_NAME_START COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern char PROF_NAME_STOP COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern ValueProfNode PROF_VNODES_START COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern ValueProfNode PROF_VNODES_STOP COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;

COMPILER_RT_VISIBILITY const __llvm_profile_data *__llvm_profile_begin_data(void) { return &PROF_DATA_START; }
COMPILER_RT_VISIBILITY const __llvm_profile_data *__llvm_profile_end_data(void) { return &PROF_DATA_STOP; }
COMPILER_RT_VISIBILITY const char *__llvm_profile_begin_names(void) { return &PROF_NAME_START; }
COMPILER_RT_VISIBILITY const char *__llvm_profile_end_names(void) { return &PROF_NAME_STOP; }
COMPILER_RT_VISIBILITY char *__llvm_profile_begin_counters(void) { return &PROF_CNTS_START; }
COMPILER_RT_VISIBILITY char *__llvm_profile_end_counters(void) { return &PROF_CNTS_STOP; }
COMPILER_RT_VISIBILITY char *__llvm_profile_begin_bitmap(void) { return &PROF_BITS_START; }
COMPILER_RT_VISIBILITY char *__llvm_profile_end_bitmap(void) { return &PROF_BITS_STOP; }
COMPILER_RT_VISIBILITY uint32_t *__llvm_profile_begin_orderfile(void) { return &PROF_ORDERFILE_START; }

COMPILER_RT_VISIBILITY ValueProfNode *__llvm_profile_begin_vnodes(void) { return &PROF_VNODES_START; }
COMPILER_RT_VISIBILITY ValueProfNode *__llvm_profile_end_vnodes(void) { return &PROF_VNODES_STOP; }
COMPILER_RT_VISIBILITY ValueProfNode *CurrentVNode = &PROF_VNODES_START;
COMPILER_RT_VISIBILITY ValueProfNode *EndVNode = &PROF_VNODES_STOP;

/* No binary ids (a raw profile without them is valid), no write-at-exit of the stock runtime: the dump is vita_pgo.cpp's.  The instrumented code references this variable on every target that is not Linux (the "runtime hook"); defining it
 * here is what the stock InstrProfilingRuntime.cpp does, without the static initialiser that registers the atexit writer. */
COMPILER_RT_VISIBILITY int __llvm_write_binary_ids(ProfDataWriter *Writer) { (void)Writer; return 0; }
int __llvm_profile_runtime = 0;
