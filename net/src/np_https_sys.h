// np_https_sys.h: what np_https needs from the platform (sockets, name resolution, threads, clocks, random bytes), in
// one place so that np_https.cpp is the same code on the host (tests, the desktop harness) and on the Vita (sceNet).
// Internal to np_https*.cpp.
#ifndef NP_HTTPS_SYS_H
#define NP_HTTPS_SYS_H

#include <stddef.h>
#include <stdint.h>

namespace nph {

// ---- time ---------------------------------------------------------------------------------------------------------------
int64_t MonoMs();            // monotonic milliseconds
int64_t WallSeconds();       // UTC seconds since 1970 from the platform clock (the Vita's RTC); 0 if there is none
void SleepMs(uint32_t ms);
extern int64_t (*g_wall_override)(void);   // host tests: replaces WallSeconds() when non-null

// ---- log (stderr, "[net-https] ...") ---------------------------------------------------------------------------------------
void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));

// ---- a tiny lock (held for a few instructions, never across a blocking call) ---------------------------------------------------
struct SpinLock {
    volatile int v;
    void lock();
    void unlock();
};

// ---- worker thread and its wake-up -------------------------------------------------------------------------------------------
struct Event;
Event* EventCreate();
void EventDestroy(Event*);
void EventSignal(Event*);
bool EventWait(Event*, uint32_t timeout_ms);   // true if signalled (auto-reset), false on time-out

struct Thread;
Thread* ThreadStart(void (*entry)(void*), void* arg);   // null on failure
bool ThreadJoin(Thread*, uint32_t timeout_ms);          // true if it ended and was reclaimed; false: still running (leave it)

// ---- network --------------------------------------------------------------------------------------------------------------------
enum { NET_OK = 0, NET_TIMEOUT = 1, NET_CANCELLED = 2, NET_FAIL = -1, NET_EOF = -2, NET_WANT = 0 };

bool NetworkUp();                                    // the console has a network connection (always true on the host)
// IPv4 only. 0 = ok (*ip in host byte order); NET_TIMEOUT, NET_CANCELLED, NET_FAIL (*sys = the system's code).
int Resolve(const char* host, uint32_t* ip, uint32_t timeout_ms, const volatile int* cancel, int* sys);
void ResolveAbort();                                 // from any thread: makes a Resolve in progress return soon (cancelled); harmless otherwise

struct Sock {
    int fd;
    int ep;     // epoll id (Vita), -1 otherwise
    int pool;   // whose budget the socket counts against (SockPool); SockInit sets SOCK_POOL_HTTPS
};
void SockInit(Sock* s);
// Non-blocking connect with a time limit. 0 = connected; NET_TIMEOUT, NET_CANCELLED, NET_FAIL (*sys).
int SockConnect(Sock* s, uint32_t ip, uint16_t port, uint32_t timeout_ms, const volatile int* cancel, int* sys);
// Waits for readability (or writability) for up to ms. 1 = ready, 0 = time-out, -1 = error.
int SockWait(Sock* s, bool for_write, uint32_t ms);
// >0 bytes moved, 0 = would block, -1 = error (*sys), and for SockRecv -2 = the peer closed.
int SockSend(Sock* s, const void* buf, size_t len, int* sys);
int SockRecv(Sock* s, void* buf, size_t len, int* sys);
void SockClose(Sock* s);

// The socket budget (netport31, from Strikers onmatch23): the console's socket table is small and shared with the system and other
// applications (an FTP server wedged with "425 Cannot open data connection" after a socket-heavy online run on the Strikers console).
// Every socket (and, on the Vita, its epoll id) made or closed here is counted per pool. The HTTPS client holds ONE connection
// (np_https.cpp g_conn) and the UPnP client one exchange at a time, so a pool never needs more than one socket: SockConnect refuses a
// new one (NET_FAIL, *sys = kSockBudgetRefused) while kMaxSocketsPerPool are still open, with one log line, so a leak shows as a refused
// request instead of draining the table. Counts are process totals; readable from any thread.
enum SockPool { SOCK_POOL_HTTPS = 0, SOCK_POOL_UPNP = 1, SOCK_POOLS = 2 };
enum { kMaxSocketsPerPool = 2, kSockBudgetRefused = -4 };
struct SockCounts { int live, epolls, opened, closed, refused, most; };
SockCounts SockStats(int pool);

}  // namespace nph

#endif
