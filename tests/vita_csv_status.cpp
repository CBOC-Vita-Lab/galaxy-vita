// Run actual export-status functions with VitaSDK Newlib, not the host formatter.
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <reent.h>
#include <sys/stat.h>

namespace diag { constexpr bool kEnabled=true; }
constexpr bool kBackendDiagnostics=true;
struct Log {
    bool frozen=false;
    size_t bytes;
    unsigned dropped;
    int result;
    Log& Capture() { return *this; }
    bool Frozen() const { return frozen; }
    size_t ExportedBytes() const { return bytes; }
    unsigned Dropped() const { return dropped; }
    int ExportPaused() { frozen=true; return result; }
};
Log frameLog{false,488930,7,-17},g_glStats{false,368884,3,0};
char line[256];
struct _reent reentrant = _REENT_INIT(reentrant);
#define VGX_LOG(...) _snprintf_r(&reentrant,line,sizeof line,"[vgx] " __VA_ARGS__)
#include "csv-status-functions.inc"

void RawWrite(const char* text) {
    const unsigned length=std::strlen(text);
    register unsigned r0 asm("r0")=1;
    register const char* r1 asm("r1")=text;
    register unsigned r2 asm("r2")=length;
    register unsigned r7 asm("r7")=4;
    asm volatile("svc 0" : "+r"(r0) : "r"(r1),"r"(r2),"r"(r7) : "memory");
}
extern "C" void* _sbrk(ptrdiff_t size) {
    alignas(8) static char heap[65536]; static size_t used=0;
    if (size<0 || static_cast<size_t>(size)>sizeof heap-used) return reinterpret_cast<void*>(-1);
    void* result=heap+used; used+=(static_cast<size_t>(size)+7)&~size_t(7); return result;
}
// Single-threaded formatter fixture: substitute platform services, not Newlib's parser.
extern "C" struct _reent* __getreent() { return &reentrant; }
extern "C" void __malloc_lock(struct _reent*) {}
extern "C" void __malloc_unlock(struct _reent*) {}
extern "C" void* _sbrk_r(struct _reent*,ptrdiff_t size) { return _sbrk(size); }
extern "C" void* sceClibMemcpy(void* dst,const void* src,size_t n) {
    auto* d=static_cast<volatile unsigned char*>(dst);
    const auto* s=static_cast<const unsigned char*>(src);
    for (size_t i=0;i<n;++i) d[i]=s[i];
    return dst;
}
[[noreturn]] void UnexpectedIo() {
    RawWrite("FAIL: string-only Newlib formatter attempted an OS service\n");
    register unsigned r0 asm("r0")=250,r7 asm("r7")=1;
    asm volatile("svc 0" : : "r"(r0),"r"(r7) : "memory");
    __builtin_unreachable();
}
extern "C" _ssize_t _write_r(struct _reent*,int,const void*,size_t) { UnexpectedIo(); }
extern "C" _ssize_t _read_r(struct _reent*,int,void*,size_t) { UnexpectedIo(); }
extern "C" _off_t _lseek_r(struct _reent*,int,_off_t,int) { UnexpectedIo(); }
extern "C" int _close_r(struct _reent*,int) { UnexpectedIo(); }
extern "C" int _fstat_r(struct _reent*,int,struct stat*) { UnexpectedIo(); }
extern "C" int _isatty_r(struct _reent*,int) { UnexpectedIo(); }
extern "C" int _getpid_r(struct _reent*) { UnexpectedIo(); }
extern "C" int _kill_r(struct _reent*,int,int) { UnexpectedIo(); }
extern "C" [[noreturn]] void _exit(int) { UnexpectedIo(); }
extern "C" [[noreturn]] void _start() {
    int result=0;
    if (ExportFrameLog()!=-17 || std::strcmp(line,"[vgx] RAM frontend CSV: 488930 bytes retained, 7 rows dropped, export result -17\n")) result=1;
    if (result) { RawWrite("FAIL: Vita Newlib frontend status: "); RawWrite(line); }
    if (ExportCountLog()!=0 || std::strcmp(line,"[vgx] RAM backend CSV: 368884 bytes retained, 3 rows dropped, export result 0\n")) {
        result=1; RawWrite("FAIL: Vita Newlib backend status: "); RawWrite(line);
    }
    if (!result) RawWrite("PASS: actual export status functions preserve bytes, dropped rows and signed result with VitaSDK Newlib\n");
    register int r0 asm("r0")=result;
    register unsigned r7 asm("r7")=1;
    asm volatile("svc 0" : : "r"(r0),"r"(r7) : "memory");
    __builtin_unreachable();
}
