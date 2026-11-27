// tls40: host test of the pure part of the fast TLS lookup (vita/vgx-game/fast_tls_walk.h): the key-list walk behind pthread_getspecific
// on the vitasdk, with a fake reent whose list head sits 8 bytes before it.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../vita/vgx-game/fast_tls_walk.h"
static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
struct FakeReent { alignas(8) unsigned char block[8 + 512]; };   // [0,8): the TLS header, the head pointer in its first word; the reent proper at +8
int main() {
    FakeReent fr; std::memset(&fr, 0, sizeof fr);
    const void* reent = fr.block + FAST_TLS_REENT_HEAD_OFFSET;
    fast_tls_node* head = nullptr;
    int v1, v2, v3;
    CHECK(fast_tls_walk(reent, 5) == nullptr);                       // empty list
    fast_tls_node n3{nullptr, 9, &v3}, n2{&n3, 7, &v2}, n1{&n2, 5, &v1};
    head = &n1; std::memcpy(fr.block, &head, sizeof head);
    CHECK(fast_tls_walk(reent, 5) == &v1);
    CHECK(fast_tls_walk(reent, 7) == &v2);
    CHECK(fast_tls_walk(reent, 9) == &v3);                           // last node
    CHECK(fast_tls_walk(reent, 6) == nullptr);                       // absent key
    n2.value = nullptr;
    CHECK(fast_tls_walk(reent, 7) == nullptr);                       // a key set to NULL reads NULL
    CHECK(sizeof(fast_tls_node) == 3 * sizeof(void*) && offsetof(fast_tls_node, key) == sizeof(void*));
    // imp45: the measured slot offset must be one of the two known layouts (the firmware keeps the slots 0x800 below the thread pointer)
    CHECK(fast_tls_delta_ok(4 * 0x89 - 0x800));                      // the console: sceKernelGetTLSAddr(n) = TPIDRURO - 0x800 + 4n
    CHECK(fast_tls_delta_ok(4 * 0x89));                              // slots at the thread pointer
    CHECK(!fast_tls_delta_ok(0) && !fast_tls_delta_ok(4 * 0x88 - 0x800) && !fast_tls_delta_ok(4 * 0x89 - 0x400) && !fast_tls_delta_ok(-(4 * 0x89)));
    CHECK(4 * 0x89 - 0x800 == -1500);                                // (what the console's "fasttls:" line must print)
    // fix47: the OS key is measured from the node that holds a unique value; a pthread_key_t is a HANDLE whose first word is the OS key
    // (libpthread.a pthread_getspecific: ldr r0,[r0,#0]; b pte_osTlsGetValue). A model of the library on two threads' lists:
    {
        struct Handle { unsigned key; void* destructor; void* lock; void* threads; } h{0x2a, nullptr, nullptr, nullptr};   // pthread_key_t = &h
        auto lib_get = [](const void* r, const Handle* k) { return fast_tls_walk(r, k->key); };                            // pthread_getspecific
        FakeReent main_r, second_r; std::memset(&main_r, 0, sizeof main_r); std::memset(&second_r, 0, sizeof second_r);
        const void* mr = main_r.block + FAST_TLS_REENT_HEAD_OFFSET; const void* sr = second_r.block + FAST_TLS_REENT_HEAD_OFFSET;
        int probe, mark, other;
        fast_tls_node m2{nullptr, h.key, &probe}, m1{&m2, 3, &other};         // main: an older key, then the test key
        fast_tls_node* mh = &m1; std::memcpy(main_r.block, &mh, sizeof mh);
        uintptr_t oskey = 0;
        CHECK(fast_tls_key_of_value(mr, &probe, &oskey) && oskey == h.key);   // measured, not read from the handle
        CHECK(oskey != (uintptr_t)&h);
        CHECK(fast_tls_walk(sr, oskey) == nullptr && lib_get(sr, &h) == nullptr);   // second thread before it set the key: NULL on both, main's value not seen
        fast_tls_node s1{nullptr, h.key, &mark};
        fast_tls_node* sh = &s1; std::memcpy(second_r.block, &sh, sizeof sh);
        CHECK(lib_get(sr, &h) == &mark);
        CHECK(fast_tls_walk(sr, oskey) == &mark);                             // the fixed check: walk with the OS key = the library
        CHECK(fast_tls_walk(sr, (uintptr_t)&h) == nullptr);                   // imp45's check: walk with the handle finds nothing -> "key walk differs" on a correct layout
        uintptr_t k2 = 0;
        CHECK(fast_tls_key_of_value(sr, &mark, &k2) && k2 == oskey);
        CHECK(fast_tls_walk(mr, oskey) == &probe);                            // threads keep their own values
        // the measurement refuses what it cannot be sure of: no node, or two nodes, with the value
        uintptr_t k3 = 77;
        CHECK(!fast_tls_key_of_value(sr, &probe, &k3));
        fast_tls_node d2{nullptr, 8, &probe}, d1{&d2, 9, &probe};
        fast_tls_node* dh = &d1; std::memcpy(second_r.block, &dh, sizeof dh);
        CHECK(!fast_tls_key_of_value(sr, &probe, &k3));
        // a wrong layout (head read from the wrong place = an empty or foreign list) fails the measurement, so the feature stays off
        FakeReent wrong; std::memset(&wrong, 0, sizeof wrong);
        CHECK(!fast_tls_key_of_value(wrong.block + FAST_TLS_REENT_HEAD_OFFSET, &probe, &k3));
    }
    if (!fails) std::printf("fast_tls_walk: ok\n");
    return fails != 0;
}
