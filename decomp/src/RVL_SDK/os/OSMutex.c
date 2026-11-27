#include "revolution/os.h"

#if defined(__vita__)
#include "port/port.h"

static void traceMutexContext(const char* operation, OSMutex* mutex, OSThread* current, void* caller) {
    // (GXV_LC_MUTEX_FAST=1: no check, see port_lc_mutex_fast; the TLS read is the cost)
    if (current != NULL && (port_lc_mutex_fast() || port_is_game_thread())) return;
    static unsigned reported;
    if (__atomic_fetch_add(&reported, 1, __ATOMIC_RELAXED) < 4) {
        port_log("native-mutex context op=%s mutex=%p current=%p caller=%p guest=%d owns=%d",
            operation, (void*)mutex, (void*)current, caller, port_is_game_thread(), port_owns_game_cpu());
    }
}
#endif

#define EnqueueTail(queue, mutex, link)         \
do {                                            \
    OSMutex* __prev;                            \
                                                \
    __prev = (queue)->tail;                     \
    if (__prev == NULL)                         \
        (queue)->head = (mutex);                \
    else                                        \
        __prev->link.next = (mutex);            \
    (mutex)->link.prev = __prev;                \
    (mutex)->link.next = NULL;                  \
    (queue)->tail = (mutex);                    \
} while (0)

#define DequeueItem(queue, mutex, link)         \
do {                                            \
    OSMutex* __next;                            \
    OSMutex* __prev;                            \
                                                \
    __next = (mutex)->link.next;                \
    __prev = (mutex)->link.prev;                \
                                                \
    if (__next == NULL)                         \
        (queue)->tail = __prev;                 \
    else                                        \
        __next->link.prev = __prev;             \
                                                \
    if (__prev == NULL)                         \
        (queue)->head = __next;                 \
    else                                        \
        __prev->link.next = __next;             \
} while (0)

#define DequeueHead(queue, mutex, link)         \
do {                                            \
    OSMutex* __next;                            \
                                                \
    (mutex) = (queue)->head;                    \
    __next = (mutex)->link.next;                \
    if (__next == NULL)                         \
        (queue)->tail = NULL;                   \
    else                                        \
        __next->link.prev = NULL;               \
    (queue)->head = __next;                     \
} while (0)

void OSInitMutex(OSMutex* mutex) {
    OSInitThreadQueue(&mutex->queue);
    mutex->thread = NULL;
    mutex->count = 0;
}

void OSLockMutex(OSMutex* mutex) {
    BOOL enabled;
    OSThread* currentThread, *ownerThread;

    enabled = OSDisableInterrupts();
    currentThread = OSGetCurrentThread();

#if defined(__vita__)
    traceMutexContext("lock", mutex, currentThread, __builtin_return_address(0));
#endif

    for (;;) {
        ownerThread = ((volatile OSMutex*)mutex)->thread;

        if (ownerThread == NULL) {
            mutex->thread = currentThread;
            mutex->count++;
            EnqueueTail(&currentThread->queueMutex, mutex, link);
            break;
        }
        else if (ownerThread == currentThread) {
            mutex->count++;
            break;
        }
        else {
            currentThread->mutex = mutex;
            __OSPromoteThread(mutex->thread, currentThread->priority);
            OSSleepThread(&mutex->queue);
            currentThread->mutex = NULL;
        }
    }

    OSRestoreInterrupts(enabled);
}

void OSUnlockMutex(OSMutex* mutex) {
    BOOL enabled;
    OSThread* currentThread;

    enabled = OSDisableInterrupts();
    currentThread = OSGetCurrentThread();

#if defined(__vita__)
    traceMutexContext("unlock", mutex, currentThread, __builtin_return_address(0));
#endif

    if (mutex->thread == currentThread && --mutex->count == 0) {
        DequeueItem(&currentThread->queueMutex, mutex, link);
        mutex->thread = NULL;
        if (currentThread->priority < currentThread->base) {
            currentThread->priority = __OSGetEffectivePriority(currentThread);
        }

        OSWakeupThread(&mutex->queue);
    }

    OSRestoreInterrupts(enabled);
}

#if defined(__vita__)
// defaults48 (GXV_MUTEX_GATE_FAST=1, platform/src/os/os_thread.cpp port_mutex_gate_fast: why this is safe and when it applies).
// OSLockMutex / OSUnlockMutex without their interrupt-disable section, for the cases in which they touch nothing but the mutex and the
// calling thread's own mutex list: 1 = done (the fields are as the game's own function leaves them), 0 = not done, the caller calls
// the game's function. No safe point inside (no call that can switch guest threads or take an interrupt).
extern int port_mutex_gate_fast(void) __attribute__((weak));
int port_mutex_lock_gate_fast(OSMutex* mutex) {
    OSThread* currentThread;
    OSThread* ownerThread;
    if (!port_mutex_gate_fast || !port_mutex_gate_fast()) return 0;
    currentThread = OSGetCurrentThread();
    if (currentThread == NULL) return 0;
    ownerThread = mutex->thread;
    if (ownerThread == NULL) {
        mutex->thread = currentThread;
        mutex->count++;
        EnqueueTail(&currentThread->queueMutex, mutex, link);
        return 1;
    }
    if (ownerThread == currentThread) {
        mutex->count++;
        return 1;
    }
    return 0;  // (another thread holds it: the game's function sleeps)
}
int port_mutex_unlock_gate_fast(OSMutex* mutex) {
    OSThread* currentThread;
    if (!port_mutex_gate_fast || !port_mutex_gate_fast()) return 0;
    currentThread = OSGetCurrentThread();
    if (currentThread == NULL || mutex->thread != currentThread || mutex->count <= 0) return 0;
    if (mutex->count > 1) {
        mutex->count--;
        return 1;
    }
    // (the last unlock: only when nobody waits for it and the caller's priority was not raised by a waiter; else the game's function
    // wakes the waiter and works the priority out)
    if (mutex->queue.head != NULL || currentThread->priority < currentThread->base) return 0;
    mutex->count = 0;
    DequeueItem(&currentThread->queueMutex, mutex, link);
    mutex->thread = NULL;
    return 1;
}
#endif

void __OSUnlockAllMutex(OSThread* thread) {
    OSMutex* mutex;
    while (thread->queueMutex.head) {
        DequeueHead(&thread->queueMutex, mutex, link);
        ASSERT(mutex->thread == thread);
        mutex->count = 0;
        mutex->thread = NULL;
        OSWakeupThread(&mutex->queue);
    }
}

BOOL OSTryLockMutex(OSMutex* mutex) {
    BOOL enabled, locked;
    OSThread* currentThread;

    enabled = OSDisableInterrupts();
    currentThread = OSGetCurrentThread();

#if defined(__vita__)
    traceMutexContext("try-lock", mutex, currentThread, __builtin_return_address(0));
#endif

    if (mutex->thread == NULL) {
        mutex->thread = currentThread;
        mutex->count++;
        EnqueueTail(&currentThread->queueMutex, mutex, link);
        locked = TRUE;
    }
    else if (mutex->thread == currentThread) {
        mutex->count++;
        locked = TRUE;
    }
    else {
        locked = FALSE;
    }

    OSRestoreInterrupts(enabled);
    return locked;
}
