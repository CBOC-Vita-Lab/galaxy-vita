// fast_tls (tls40): shorter paths for the two calls the vitasdk's emulated thread-local storage is built on (MH3 Vita port,
// vita-tls-lookups.md: 30 instructions and a 192-byte frame down to about 15). Linked with -Wl,--wrap=__getreent -Wl,--wrap=pte_osTlsGetValue
// (vita/vgx-game/CMakeLists.txt, FAST_TLS=ON, the default). Everything that reaches them is covered, on every thread (the game's, vitaGL's,
// audio, workers, any sceKernelCreateThread thread): errno, malloc/stdio's _REENT, pthread_getspecific / pthread_self and the compiler's
// __emutls_get_address (thread_local).
//  * __getreent: newlib's __getreent_for_thread(0) reads the thread's _reent* from TLS slot 0x89 (sceKernelGetTLSAddr(0x89)), and only when it is
//    0 (a thread's first call) takes a mutex and builds one. The wrap reads the slot straight from TPIDRURO (the TLS block, slot n at +4n)
//    and returns it when set; when it is 0 it calls the real function, which makes it.
//  * pte_osTlsGetValue: the real one gets the reent through vitasdk_get_tls_data and walks the key list; the wrap does the same walk with the
//    reent from the slot read above; reent 0 goes to the real function.
// Safety: the wraps do nothing until port_fast_tls_init (main thread, before any other thread) has checked the layout on the live console:
// the slot's place from TPIDRURO is MEASURED (sceKernelGetTLSAddr(0x89) - TPIDRURO) and must be one of the two known layouts (fast_tls_walk.h
// fast_tls_delta_ok: the firmware's TPIDRURO - 0x800 + 4*0x89, or TPIDRURO + 4*0x89); the slot must hold what __real___getreent returns; for a test
// key the walk must give what the real pte_osTlsGetValue gives; and a second thread, made for the check, must find the same offset, its own reent in
// its own slot and its own key value by the walk. Any mismatch leaves it off (every call then goes to the real function) and says which check it was
// (port_fast_tls_reason, the "fasttls:" line).
// fix47 (22 Nov): the second thread's check walks with the OS key (measured: fast_tls_key_of_value), not the pthread_key_t handle. imp45's check
// passed the handle (a struct pthread_key_t_*, whose first word is the OS key) to pte_osTlsGetValue and to the walk: both gave NULL, so a correct
// layout was refused ("a second thread's key walk differs from the library's", console c35t3 and Vita3K).
// imp45 (22 Nov): the first version required TPIDRURO + 4*0x89 == sceKernelGetTLSAddr(0x89), which is not the firmware's layout: it was off in every
// console run.
// GXV_FAST_TLS=0 in env.txt: off (A/B; read after env.txt is loaded, port_fast_tls_env). port_fast_tls_status(): 0 not initialised, 1 on, 2 off by env, 3 off: layout check failed.
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <psp2/kernel/threadmgr.h>
#include <sys/reent.h>
#include "fast_tls_walk.h"

extern struct _reent* __real___getreent(void);
extern void* __real_pte_osTlsGetValue(unsigned int key);

static volatile int sOn = 0;
static volatile int sStatus = 0;
static intptr_t sDelta = 0;          /* the slot's place from TPIDRURO, measured by port_fast_tls_init before sOn is ever 1 */
static const char* sReason = "";
static intptr_t sSeenDelta = 0;

static inline char* thread_pointer(void) {
    char* tp;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp));   /* TPIDRURO */
    return tp;
}
static inline struct _reent* fast_reent(void) {
    return *(struct _reent**)(thread_pointer() + sDelta);
}

struct _reent* __wrap___getreent(void) {
    if (__builtin_expect(sOn, 1)) {
        struct _reent* r = fast_reent();
        if (__builtin_expect(r != NULL, 1)) return r;
    }
    return __real___getreent();
}

void* __wrap_pte_osTlsGetValue(unsigned int key) {
    if (__builtin_expect(sOn, 1)) {
        struct _reent* r = fast_reent();
        if (__builtin_expect(r != NULL, 1)) return fast_tls_walk(r, key);
    }
    return __real_pte_osTlsGetValue(key);
}

int port_fast_tls_status(void) { return sStatus; }
/* Why it is off (status 3), or "" ; and the offset seen (for the log line). */
const char* port_fast_tls_reason(void) { return sReason; }
int port_fast_tls_delta(void) { return (int)sSeenDelta; }

/* The second thread's check (sOn is 0 while it runs: every library call it makes takes the real path). */
/* fix47: the walk is given the OS key (measured on the main thread, fast_tls_key_of_value), not the pthread_key_t handle: imp45 passed the handle
   to both __real_pte_osTlsGetValue and the walk, no node has a handle for a key, both gave NULL, and a correct layout was refused on the console and in
   Vita3K alike ("a second thread's key walk differs from the library's"). */
struct fast_tls_probe { pthread_key_t key; uintptr_t oskey; int ok; const char* why; };
static void* fast_tls_probe_thread(void* arg) {
    struct fast_tls_probe* p = (struct fast_tls_probe*)arg;
    static int mark;
    char* tp = thread_pointer();
    void** slot = (void**)sceKernelGetTLSAddr(FAST_TLS_REENT_SLOT);
    struct _reent* real = __real___getreent();
    if (!tp || !slot || (char*)slot - tp != sDelta) { p->why = "a second thread has its slot at another offset"; return NULL; }
    if (!real || *slot != (void*)real) { p->why = "a second thread's slot does not hold its reent"; return NULL; }
    /* a key this thread never set (the main thread's value must not show here): NULL by the library and by the walk */
    if (pthread_getspecific(p->key) != NULL || fast_tls_walk(*(struct _reent**)(tp + sDelta), p->oskey) != NULL) {
        p->why = "a second thread sees a key value before it set one"; return NULL;
    }
    if (pthread_setspecific(p->key, &mark) != 0) { p->why = "a second thread could not set the test key"; return NULL; }
    {
        void* lib = pthread_getspecific(p->key);                         /* the library's whole path (sOn is 0) */
        void* os = __real_pte_osTlsGetValue((unsigned int)p->oskey);      /* the wrapped function itself, with the OS key */
        void* walk = fast_tls_walk(*(struct _reent**)(tp + sDelta), p->oskey);
        uintptr_t k2 = 0;
        if (lib != (void*)&mark) { p->why = "a second thread could not read the test key back"; return NULL; }
        if (os != lib) { p->why = "a second thread: the measured OS key is not the one the library uses"; return NULL; }
        if (walk != lib) { p->why = "a second thread's key walk differs from the library's"; return NULL; }
        if (!fast_tls_key_of_value(*(struct _reent**)(tp + sDelta), &mark, &k2) || k2 != p->oskey) { p->why = "a second thread keeps the test key under another OS key"; return NULL; }
    }
    if (pthread_setspecific(p->key, NULL) != 0 || pthread_getspecific(p->key) != NULL || fast_tls_walk(*(struct _reent**)(tp + sDelta), p->oskey) != NULL) {
        p->why = "a second thread: a cleared key still reads a value"; return NULL;
    }
    p->ok = 1;
    return NULL;
}

void port_fast_tls_init(void) {
    {
        char* tp = thread_pointer();
        void** slot = (void**)sceKernelGetTLSAddr(FAST_TLS_REENT_SLOT);
        struct _reent* real = __real___getreent();   /* makes this thread's reent if it had none */
        if (!tp || !slot) { sStatus = 3; sReason = "no thread pointer or no slot address"; goto done; }
        sSeenDelta = (char*)slot - tp;
        if (!fast_tls_delta_ok(sSeenDelta)) { sStatus = 3; sReason = "the slot is at an offset from the thread pointer that is not a known layout"; goto done; }
        if (!real || *slot != (void*)real) { sStatus = 3; sReason = "the slot does not hold the reent"; goto done; }
        sDelta = sSeenDelta;
        pthread_key_t k;
        if (pthread_key_create(&k, NULL) != 0) { sStatus = 3; sReason = "no test key"; goto done; }
        static int probe;
        uintptr_t oskey = 0;
        int ok = pthread_setspecific(k, &probe) == 0;
        /* fix47: the OS key behind the handle, measured (the one node of this thread's list that holds &probe), and proven against the library */
        if (ok && !(fast_tls_key_of_value(real, &probe, &oskey) && __real_pte_osTlsGetValue((unsigned int)oskey) == (void*)&probe)) {
            pthread_key_delete(k); sStatus = 3; sReason = "the test key's OS key could not be measured"; goto done;
        }
        if (ok) {
            sOn = 0; void* a = pthread_getspecific(k);   /* real path */
            sOn = 1; void* b = pthread_getspecific(k);   /* fast path */
            sOn = 0;
            ok = a == (void*)&probe && b == a;
            /* a key this thread never set must give NULL on both */
            if (ok) { pthread_setspecific(k, NULL); sOn = 1; ok = pthread_getspecific(k) == NULL; sOn = 0; }
        }
        if (!ok) { pthread_key_delete(k); sStatus = 3; sReason = "the key walk differs from the library's"; goto done; }
        {   /* the same on a second thread: the offset is the kernel's for every thread, not this one's alone */
            struct fast_tls_probe p = {k, oskey, 0, "a second thread could not be made for the check"};
            pthread_t t;
            if (pthread_create(&t, NULL, fast_tls_probe_thread, &p) == 0) pthread_join(t, NULL);
            pthread_key_delete(k);
            if (!p.ok) { sStatus = 3; sReason = p.why; goto done; }
        }
        sOn = 1; sStatus = 1;
    }
done:
    return;
}

/* After env.txt is loaded (entry.cpp): GXV_FAST_TLS=0 switches it off. Safe with other threads running: both paths give the same answers. */
void port_fast_tls_env(void) {
    const char* e = getenv("GXV_FAST_TLS");
    if (e && e[0] == '0' && sStatus == 1) { sOn = 0; sStatus = 2; }
}
