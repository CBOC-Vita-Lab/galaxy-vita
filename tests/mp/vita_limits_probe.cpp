// Probe for tests/mp/vita_limits_test.sh: links platform/src/os/os_mem.cpp alone and calls port_mem_init, so the test can read which bank sizes
// the desktop build maps with and without PETARI_VITA_LIMITS=1 (the "mem: native MEM1 ... MEM2 ..." line). Not part of any game build.
#include <cstdio>
#include <cstdint>
#include "port/port.h"
#include "revolution/os.h"
extern "C" int port_mem_init(void);
int main(){ port_mem_init(); return 0; }
extern "C" void port_vgx_notify_write(const void*, size_t) {}
#include <cstdarg>
#include <cstdlib>
extern "C" void port_log(const char* f, ...) { va_list a; va_start(a,f); printf("log: "); vprintf(f,a); printf("\n"); va_end(a); }
extern "C" void port_fatal(const char* f, ...) { va_list a; va_start(a,f); vprintf(f,a); va_end(a); exit(2); }
