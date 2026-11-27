// fast_tls (tls40): the pure part of the fast pthread-key lookup, shared by vita/vgx-game/fast_tls.c and tests/fast_tls_walk.cpp.
// pthread-embedded on the vitasdk keeps a thread's key values in a singly linked list whose head sits 8 bytes before the thread's
// newlib _reent (vitasdk_get_tls_data(0) = __getreent_for_thread(0) - 8); a node is {next, key, value} (libpthread's pte_osTlsGetValue
// walks it: ldr r0,[head]; loop: key at +4 compared, value at +8, next at +0). Layout read from the SDK's libc.a / libpthread.a disassembly.
#ifndef FAST_TLS_WALK_H
#define FAST_TLS_WALK_H
#include <stddef.h>
#include <stdint.h>
struct fast_tls_node { struct fast_tls_node* next; uintptr_t key; void* value; };
#define FAST_TLS_REENT_HEAD_OFFSET 8u   /* head pointer lives at (char*)reent - 8 */
#define FAST_TLS_REENT_SLOT 0x89u       /* TLS slot (sceKernelGetTLSAddr key) that holds the thread's _reent* */
/* imp45 (22 Nov): where the slot sits from the thread pointer (TPIDRURO). The first version assumed slot n at TPIDRURO + 4n and its layout check
   failed on every console run ("fasttls: off (layout check failed)", console34): the firmware keeps the 0x100 slots in the 0x800 bytes BELOW the
   thread pointer (sceKernelGetTLSAddr(n) = TPIDRURO - 0x800 + 4n). The offset is now MEASURED on the live console (sceKernelGetTLSAddr(0x89) - TPIDRURO)
   and used only if it is one of the two layouts known (the firmware's, and the slots-at-the-pointer one an emulator may have), on the main thread and
   on a second thread alike. Anything else: off. */
#define FAST_TLS_BLOCK_BYTES 0x800
static inline int fast_tls_delta_ok(intptr_t delta) {
    return delta == (intptr_t)(4u * FAST_TLS_REENT_SLOT) - FAST_TLS_BLOCK_BYTES || delta == (intptr_t)(4u * FAST_TLS_REENT_SLOT);
}
static inline void* fast_tls_walk(const void* reent, uintptr_t key) {
    const struct fast_tls_node* n = *(struct fast_tls_node* const*)((const char*)reent - FAST_TLS_REENT_HEAD_OFFSET);
    for (; n != NULL; n = n->next)
        if (n->key == key) return n->value;
    return NULL;   /* same as pte_osTlsGetValue: no node, no value */
}
/* fix47 (22 Nov): the OS key of a pthread key, MEASURED: pthread_key_t on the vitasdk is a handle (struct pthread_key_t_*) whose first word is the
   key pte_osTlsGetValue takes (pthread_getspecific: ldr r0,[r0,#0]; b pte_osTlsGetValue). The layout check must walk with the OS key, not the
   handle (imp45's second-thread check passed the handle and so failed on a correct layout). After pthread_setspecific(k, unique address) on this
   thread, the node holding that address carries the OS key. Returns 1 and the key when exactly one node holds the value; 0 otherwise. */
static inline int fast_tls_key_of_value(const void* reent, const void* value, uintptr_t* key) {
    const struct fast_tls_node* n = *(struct fast_tls_node* const*)((const char*)reent - FAST_TLS_REENT_HEAD_OFFSET);
    int found = 0, guard = 0;
    for (; n != NULL && guard < 4096; n = n->next, ++guard)
        if (n->value == value) { if (found++ == 0) *key = n->key; }
    return found == 1 && n == NULL;
}
#endif
