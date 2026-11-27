// Galaxy 3.0 multiplayer: the online service client. See mp_service.h and server/SMG-PROTOCOL.txt.
//
// Game-thread module: every entry point runs on the game thread; requests run on np_https's worker thread (https://) or, in desktop
// test builds only, on a small detached thread speaking plain HTTP to the local fake server (http://). Fixed request slots, fixed
// result tables; JSON is parsed only when an answer arrives (a few times a minute at most), never per frame.
// integ: port-side storage must never come from the game's JKR heaps (operator new on a game thread uses the current scene heap,
// which is destroyed at the next scene change while the session's objects live on): every entry point the game calls is a host
// allocation scope (port/heap_routing.h; referenced weakly so the host tests link without the port).
extern "C" void port_host_alloc_scope_enter(void) __attribute__((weak));
extern "C" void port_host_alloc_scope_leave(void) __attribute__((weak));
namespace {
struct MpHostAllocScope {
    MpHostAllocScope() { if (port_host_alloc_scope_enter) port_host_alloc_scope_enter(); }
    ~MpHostAllocScope() { if (port_host_alloc_scope_leave) port_host_alloc_scope_leave(); }
};
}  // namespace
#define MP_HOST_ALLOC MpHostAllocScope mpHostAllocScope_
#include "mp_service.h"
#include "mp_svc_busy.h"   // quota32: the service / Cloudflare is out of quota: "busy right now"
#include "port/mp_invite.h"   // code5

#include "mp_config.h"
#include "mp_posix_io.h"   // netfix B8b
#include "mp_update.h"   // release3
#include "mp_name_filter.h"   // the same word list and rules as the service
#include "mp_pow.h"      // service review: the registration proof of work
#include "mp_savefile.h"          // keepfix: mp_save_recover_with (a cut account write)
#include "port/mp_keepsafe.h"     // keepfix: mp_keepsafe_account_store
// core2: the session announces this console's code in rooms without the service (port/mp_core_game.h); weak: service tests without a core.
// linkfix: the definition (mp_core.cpp) has C linkage; the header declares it so, and the weak redeclaration must too (a C++ weak
// reference is to the mangled name, which nothing defines: TellCoreCode silently did nothing). tests/mp/linkage_test.sh guards this.
#include "port/mp_core_game.h"
extern "C" void mp_core_set_own_friend_code(const char* code) __attribute__((weak));
static void TellCoreCode(const char* code) { if (mp_core_set_own_friend_code) mp_core_set_own_friend_code(code); }
extern "C" bool mp_update_scan_body(const char* body, size_t len) __attribute__((weak));   // (a test program may not link mp_update.cpp)

// serverfix: the config module may not be linked into a test program (no `server` key then)
const MpConfig* mp_config(void) __attribute__((weak));
bool mp_server_url_clean(const char* in, char* out, size_t cap) __attribute__((weak));

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>

#if defined(__has_include)
#if __has_include("np_https.h")
#include "np_https.h"
#define MP_SVC_HAVE_HTTPS 1
#elif __has_include("net/np_https.h")
#include "net/np_https.h"
#define MP_SVC_HAVE_HTTPS 1
#elif __has_include("platform/sdk/net/np_https.h")
#include "platform/sdk/net/np_https.h"
#define MP_SVC_HAVE_HTTPS 1
#endif
#if __has_include("np_json.h")
#include "np_json.h"
#define MP_SVC_HAVE_JSON 1
#elif __has_include("online/np_json.h")
#include "online/np_json.h"
#define MP_SVC_HAVE_JSON 1
#elif __has_include("net/online/np_json.h")
#include "net/online/np_json.h"
#define MP_SVC_HAVE_JSON 1
#elif __has_include("platform/sdk/net/online/np_json.h")
#include "platform/sdk/net/online/np_json.h"
#define MP_SVC_HAVE_JSON 1
#endif
#endif

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#endif

#if !defined(__vita__) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0   // (macOS desktop builds: no such flag; the fake server never resets mid-request in the tests)
#endif

#ifndef SMG_APP_VERSION
#define SMG_APP_VERSION "3.0.0"
#endif
#ifndef SMG_TIME_BUILD
#define SMG_TIME_BUILD SMG_APP_VERSION   // must be in the service's SMG_TIME_BUILDS allowlist
#endif

#if defined(MP_SVC_HAVE_JSON)

// lanfix: the friends' names go to the Same Wi-Fi room list (mp_lan.cpp); weak: builds without the session layer skip it
void mp_lan_set_trusted_names(int source, const char (*names)[MP_NAME_BYTES + 1], int n) __attribute__((weak));

namespace {

// integ: the entry points are called from the game thread (menus) and from MpCore's pump (sign-in, room lookups: the game
// thread or the session's helper thread), so every one of them takes this lock. Never call into MpCore while holding it.
std::recursive_mutex g_svcMu;
struct SvcLock {
    SvcLock() { g_svcMu.lock(); }
    ~SvcLock() { g_svcMu.unlock(); }
};
// X-Msc-Build: the session fingerprint as 8 hex digits (mp_service_set_build, from MpCore); "" = not sent. The service matches
// rooms on app version AND build, so NpNet's signaling and these requests must send the same value.
char g_build[12] = {};

// ---- clock, small helpers ------------------------------------------------------------------------------------------------------
uint64_t NowMs() {
#if defined(__vita__)
    return uint64_t(sceKernelGetProcessTimeWide()) / 1000u;
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000u + uint64_t(ts.tv_nsec) / 1000000u;
#endif
}

void CopyStr(char* dst, size_t cap, const std::string& s) {
    if (!cap) return;
    size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    memcpy(dst, s.data(), n);
    dst[n] = 0;
}

uint32_t Crc32(const void* data, size_t size) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = ~0u;
    for (size_t i = 0; i < size; ++i) crc = (crc >> 8) ^ table[(crc ^ p[i]) & 0xFF];
    return ~crc;
}
void Put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// ---- the service's rules, checked locally (server/src/core/names.js, rooms.js) ---------------------------------------------------
const char kCodeAlphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";   // no I L O 0 1

bool IsAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// 2..10 characters after trimming, A-Z a-z 0-9 space . _ -, starting and ending with a letter or digit, no double spaces, at least
// two letters or digits. (Reserved words and the profanity filter are the service's: it answers bad_name with a reason.)
bool NameValid(const char* raw, char* out, size_t cap) {
    if (!raw) return false;
    while (*raw == ' ') ++raw;
    size_t n = strlen(raw);
    while (n && raw[n - 1] == ' ') --n;
    if (n < 2 || n > 10 || n + 1 > cap) return false;
    int alnum = 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = raw[i];
        if (!(IsAlnum(c) || c == ' ' || c == '.' || c == '_' || c == '-')) return false;
        if (c == ' ' && i && raw[i - 1] == ' ') return false;
        alnum += IsAlnum(c) ? 1 : 0;
    }
    if (!IsAlnum(raw[0]) || !IsAlnum(raw[n - 1]) || alnum < 2) return false;
    memcpy(out, raw, n);
    out[n] = 0;
    return true;
}

// Friend code "XXXX-XXXX" (dash, spaces and case optional) -> "XXXX-XXXX". False when it cannot be one.
bool FriendCodeValid(const char* raw, char out[10]) {
    if (!raw) return false;
    char k[9];
    int n = 0;
    for (; *raw; ++raw) {
        char c = *raw;
        if (c == '-' || c == ' ') continue;
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if (n >= 8 || !strchr(kCodeAlphabet, c) || !c) return false;
        k[n++] = c;
    }
    if (n != 8) return false;
    memcpy(out, k, 4);
    out[4] = '-';
    memcpy(out + 5, k + 4, 4);
    out[9] = 0;
    return true;
}

bool TokenValid(const char* t) {
    size_t n = 0;
    for (; t[n]; ++n) {
        const char c = t[n];
        if (!(IsAlnum(c) || c == '_' || c == '-')) return false;
        if (n > 63) return false;
    }
    return n == 43;
}

// ---- the service address -----------------------------------------------------------------------------------------------------
// Order: env SMG_ONLINE_URL (env.txt on the Vita, the environment on the desktop), then `server = ...` of mp.cfg (a community's own deployment: https only, cleaned by
// mp_server_url_clean), then the build's NP_ONLINE_URL. mp_config() is a weak reference: a test program that does not link mp_config.cpp has no
// `server` key. The account (token) belongs to ONE address: see AccountPath().
std::string Normalize(std::string u);
// the address compiled into this build (none: ""), used to tell "our" service from another one
std::string BuildUrl() {
#if defined(NP_ONLINE_URL)
    return Normalize(NP_ONLINE_URL);
#else
    return std::string();
#endif
}
std::string ServiceUrl() {
    if (const char* e = getenv("SMG_ONLINE_URL")) if (*e) return Normalize(e);   // (ux0:data/smg/env.txt on the Vita, the environment on the desktop)
    if (mp_config && mp_server_url_clean) {
        const MpConfig* c = mp_config();
        char clean[MP_SERVER_URL_MAX + 1];
        if (c && c->server[0] && mp_server_url_clean(c->server, clean, sizeof clean)) return clean;
    }
    return BuildUrl();
}
std::string Normalize(std::string u) {
    while (!u.empty() && (u.back() == '/' || u.back() == ' ' || u.back() == '\r' || u.back() == '\n')) u.pop_back();
    if (u.compare(0, 8, "https://") == 0 && u.size() > 8) return u;
#if !defined(__vita__)
    if (u.compare(0, 7, "http://") == 0 && u.size() > 7) return u;   // the desktop tests' local fake; the console talks TLS or nothing
#endif
    return std::string();
}

// ---- the account file: <data>/online/online.dat --------------------------------------------------------------------------------
// magic "SMGO" | version u32 (1) | pid u32 | token[64] | name[20] | code[12] | crc32 of everything before. Little-endian. 112 bytes.
const uint8_t kMagic[4] = {'S', 'M', 'G', 'O'};
const uint32_t kStoreVersion = 1;
const size_t kTokenBytes = 64, kNameBytes = 20, kCodeBytes = 12;
const size_t kStoreSize = 4 + 4 + 4 + kTokenBytes + kNameBytes + kCodeBytes + 4;

struct Account {
    bool loaded = false, registered = false;
    int pid = 0;
    char token[kTokenBytes] = {};
    char name[MP_NAME_BYTES + 1] = {};
    char code[10] = {};
};
Account g_acct;

std::string OnlineDir() {
    if (const char* e = getenv("SMG_ONLINE_DIR")) if (*e) return e;
#if defined(__vita__)
    return "ux0:data/smg/online";
#else
    if (const char* d = getenv("SMG_DATA_DIR")) if (*d) return std::string(d) + "/online";
    return "smg-data/online";
#endif
}

// MP3 keepsafe: the account folder, for the copy code (version copies, Make a Copy, bringing the account back) to find the account files.
const char* OnlineDirC() {
    static std::string d;
    d = OnlineDir();
    return d.c_str();
}

void EnsureDir(const std::string& dir) {
    // the parent first (ux0:data/smg exists on the Vita; desktop runs may start from nothing)
    const size_t slash = dir.find_last_of('/');
    if (slash != std::string::npos && slash > 0) mkdir(dir.substr(0, slash).c_str(), 0777);
    mkdir(dir.c_str(), 0777);
}

// The account (and its token) belongs to ONE service address. The build's own address keeps online.dat (as before); another address (a
// community's server from mp.cfg) gets online_<8 hex of FNV-1a of the address>.dat. A token is therefore only ever read for, and sent to,
// the address it was issued by; switching `server` back and forth keeps both accounts.
std::string AccountPath(const std::string& url) {
    // (a plain http:// address only exists in the desktop tests' local fake, never on a console: it keeps online.dat)
    if (url == BuildUrl() || url.compare(0, 7, "http://") == 0) return OnlineDir() + "/online.dat";
    uint32_t h = 2166136261u;
    for (char ch : url) { h ^= uint8_t(ch); h *= 16777619u; }
    char b[32];
    snprintf(b, sizeof b, "/online_%08x.dat", unsigned(h));
    return OnlineDir() + b;
}
std::string g_acctUrl;   // the address g_acct was loaded for
bool g_hadAccount = false;   // service review: an account existed this boot: a new registration is a RE-registration (counted)
bool g_netConfigured = false;   // the online transport was handed the address and token (mp_svc_signin_poll)

bool AccountBytesValid(const uint8_t* b, uint32_t n) {
    return n == kStoreSize && !memcmp(b, kMagic, 4) && Get32(b + 4) == kStoreVersion && Get32(b + kStoreSize - 4) == Crc32(b, kStoreSize - 4);
}

void LoadAccount() {
    const std::string url = ServiceUrl();
    if (g_acct.loaded && url != g_acctUrl) {   // the address changed since (mp.cfg loaded later): this token is not for the new one
        memset(g_acct.token, 0, sizeof g_acct.token);
        g_acct = Account();
        g_netConfigured = false;
        mp_log("service address changed: account reloaded for the new address");
    }
    if (g_acct.loaded) return;
    g_acct = Account();
    g_acct.loaded = true;
    g_acctUrl = url;
    TellCoreCode("");   // core2: no account until the file says so (another address, no file, a spoiled one)
    const std::string path = AccountPath(url);
    {   // keepfix (review 10): a write that was cut leaves the new file as .mptmp or the old one as .mpbak: put a whole one back
        struct stat st;
        bool changed = false;
        if (stat(path.c_str(), &st) != 0 && errno == ENOENT) mp_save_recover_with(path.c_str(), AccountBytesValid, &changed);
        if (changed) mp_log("service account file recovered from the previous write");
    }
    uint8_t buf[kStoreSize + 1];
    const long got = mp_io_read(path.c_str(), buf, sizeof buf);   // netfix B8b: POSIX calls, no fopen beside the game's
    if (got < 0) return;
    const size_t n = static_cast<size_t>(got);
    bool ok = n == kStoreSize && !memcmp(buf, kMagic, 4) && Get32(buf + 4) == kStoreVersion && Get32(buf + kStoreSize - 4) == Crc32(buf, kStoreSize - 4);
    if (ok) {
        const uint8_t* p = buf + 12;
        g_acct.pid = int(int32_t(Get32(buf + 8)));
        memcpy(g_acct.token, p, kTokenBytes);
        g_acct.token[kTokenBytes - 1] = 0;
        p += kTokenBytes;
        char name[kNameBytes];
        memcpy(name, p, kNameBytes);
        name[kNameBytes - 1] = 0;
        p += kNameBytes;
        char code[kCodeBytes];
        memcpy(code, p, kCodeBytes);
        code[kCodeBytes - 1] = 0;
        char clean[MP_NAME_BYTES + 1];
        ok = g_acct.pid > 0 && TokenValid(g_acct.token) && NameValid(name, clean, sizeof clean) && FriendCodeValid(code, g_acct.code);
        if (ok) snprintf(g_acct.name, sizeof g_acct.name, "%s", clean);
    }
    if (!ok) {
        memset(g_acct.token, 0, sizeof g_acct.token);
        g_acct.pid = 0;
        g_acct.name[0] = g_acct.code[0] = 0;
        mp_log("service account file refused path=%s", path.c_str());
    }
    g_acct.registered = ok;
    if (ok) g_hadAccount = true;
    TellCoreCode(ok ? g_acct.code : "");
    memset(buf, 0, sizeof buf);
}

bool SaveAccount(int pid, const char* token, const char* name, const char* code) {
    const std::string dir = OnlineDir();
    EnsureDir(dir);
    const std::string path = AccountPath(g_acctUrl.empty() ? ServiceUrl() : g_acctUrl);
    uint8_t buf[kStoreSize];
    memset(buf, 0, sizeof buf);
    memcpy(buf, kMagic, 4);
    Put32(buf + 4, kStoreVersion);
    Put32(buf + 8, uint32_t(pid));
    strncpy(reinterpret_cast<char*>(buf + 12), token, kTokenBytes - 1);
    strncpy(reinterpret_cast<char*>(buf + 12 + kTokenBytes), name, kNameBytes - 1);
    strncpy(reinterpret_cast<char*>(buf + 12 + kTokenBytes + kNameBytes), code, kCodeBytes - 1);
    Put32(buf + kStoreSize - 4, Crc32(buf, kStoreSize - 4));
    // MP3 keepsafe (keepfix review 10): through the account store: the save layer's atomic writer (online.dat is never removed before the new
    // file is in place), every distinct previous credential kept (.prev, .prev.1, .prev.2; a rename keeps none), a newer game's file left
    // alone, and no new file when the old credential cannot be kept.
    const bool ok = mp_keepsafe_account_store(path.c_str(), buf, uint32_t(kStoreSize));
    memset(buf, 0, sizeof buf);
    return ok;
}

// ---- transport: one request ----------------------------------------------------------------------------------------------------
struct Answer {
    int transport;      // 0 = an HTTP answer arrived; 1 offline; 2 other transport failure; 3 the console clock is wrong (TLS dates)
    int status;
    std::string body;
};

#if !defined(__vita__)
// Plain HTTP/1.1 for the desktop tests' local fake server. One detached thread per request; state 0 running, 1 done (the owner takes
// the answer and deletes it), 2 abandoned (the thread deletes it).
struct Plain {
    std::atomic<int> state{0};
    std::string host, request;
    uint16_t port = 80;
    uint32_t timeoutMs = 15000;
    Answer res{2, 0, std::string()};
};

bool WaitFd(int fd, short ev, uint64_t deadline) {
    for (;;) {
        const uint64_t now = NowMs();
        if (now >= deadline) return false;
        pollfd p{fd, ev, 0};
        const int rc = poll(&p, 1, int(deadline - now));
        if (rc > 0) return true;
        if (rc < 0 && errno != EINTR) return false;
    }
}

void PlainRun(Plain* r) {
    const uint64_t deadline = NowMs() + r->timeoutMs;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* ai = 0;
    char port[8];
    snprintf(port, sizeof port, "%u", unsigned(r->port));
    if (getaddrinfo(r->host.c_str(), port, &hints, &ai) != 0 || !ai) { r->res.transport = 2; return; }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    bool ok = fd >= 0;
    if (ok) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        const int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        ok = rc == 0 || (errno == EINPROGRESS && WaitFd(fd, POLLOUT, deadline));
        int err = 0;
        socklen_t el = sizeof err;
        if (ok) ok = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0;
    }
    freeaddrinfo(ai);
    size_t sent = 0;
    while (ok && sent < r->request.size()) {
        if (!WaitFd(fd, POLLOUT, deadline)) { ok = false; break; }
        const ssize_t k = send(fd, r->request.data() + sent, r->request.size() - sent, MSG_NOSIGNAL);
        if (k > 0) sent += size_t(k);
        else if (k < 0 && errno != EAGAIN && errno != EINTR) ok = false;
    }
    std::string in;
    char buf[4096];
    while (ok) {   // Connection: close, so the answer ends with the connection
        if (!WaitFd(fd, POLLIN, deadline)) { ok = false; break; }
        const ssize_t k = recv(fd, buf, sizeof buf, 0);
        if (k > 0) { in.append(buf, size_t(k)); if (in.size() > 256 * 1024) ok = false; }
        else if (k == 0) break;
        else if (errno != EAGAIN && errno != EINTR) ok = false;
    }
    if (fd >= 0) close(fd);
    const size_t head = in.find("\r\n\r\n");
    if (!ok || head == std::string::npos || in.compare(0, 5, "HTTP/") != 0) { r->res.transport = 2; return; }
    const size_t sp = in.find(' ');
    r->res.status = sp == std::string::npos ? 0 : atoi(in.c_str() + sp + 1);
    std::string headers = in.substr(0, head);
    for (char& c : headers) c = char(tolower(static_cast<unsigned char>(c)));
    std::string body = in.substr(head + 4);
    if (headers.find("transfer-encoding: chunked") != std::string::npos) {
        std::string out;
        size_t pos = 0;
        for (;;) {
            const size_t eol = body.find("\r\n", pos);
            if (eol == std::string::npos) break;
            const unsigned long len = strtoul(body.c_str() + pos, 0, 16);
            // (32-bit size_t on the Vita: `eol + 2 + len` wrapped for a huge chunk size and the loop then ran on from a wrong place)
            if (!len || len > body.size() - (eol + 2)) break;
            out.append(body, eol + 2, len);
            pos = eol + 2 + len + 2;
        }
        body.swap(out);
    }
    r->res.body.swap(body);
    r->res.transport = r->res.status > 0 ? 0 : 2;
}

void* PlainThread(void* arg) {
    Plain* r = static_cast<Plain*>(arg);
    PlainRun(r);
    int expect = 0;
    if (!r->state.compare_exchange_strong(expect, 1)) delete r;   // abandoned meanwhile
    return 0;
}
#endif

// quota32: the busy gate. A "busy" answer (mp_svc_busy.h) closes the menu-side service calls (friends, board, sign-up, lookups, invites) for 60 s,
// doubling per consecutive busy answer up to 10 minutes (jittered: consoles that were told together do not come back together). While it is
// closed a call fails at once with the code "service_busy" and sends nothing, so the player gets the same plain message instead of a wait and
// the free plan is not hit by retries; the first normal answer opens it. Queued time submissions wait it out (OnSubmit) and are never dropped for it.
// /v1/info (the Connection Test, started by the player) is exempt.
uint32_t Jitter(uint32_t base);
uint64_t g_busyUntil = 0;
int g_busyStreak = 0;
uint64_t BusyLeftMs();
void BusyNote() {
    if (BusyLeftMs() > 0) return;   // fix33 (C1): answers that were already in flight when the gate closed are the same incident: one streak step per gate window
    const int sh = g_busyStreak < 4 ? g_busyStreak : 4;
    uint32_t wait = 60000u << sh;
    if (wait > 600000u) wait = 600000u;
#if !defined(__vita__)
    if (const char* t = getenv("SMG_BUSY_MS")) { const long v = atol(t); if (v >= 10 && v <= 600000) wait = uint32_t(v); }   // (tests: a short gate)
#endif
    ++g_busyStreak;
    g_busyUntil = NowMs() + Jitter(wait);
    mp_log("service busy (quota or overload): menu calls paused for %u ms (streak %d)", unsigned(g_busyUntil - NowMs()), g_busyStreak);
}
void BusyClear() { g_busyStreak = 0; g_busyUntil = 0; }
uint64_t BusyLeftMs() {
    if (!g_busyUntil) return 0;
    const uint64_t now = NowMs();
    return now < g_busyUntil && g_busyUntil - now < 3600000u ? g_busyUntil - now : 0;
}

// A request slot. kind: 0 free, 1 https (np_https handle), 2 plain.
struct Req {
    int kind = 0;
    uint32_t https = 0;
#if !defined(__vita__)
    Plain* plain = 0;
#endif
    void Drop() {
#if defined(MP_SVC_HAVE_HTTPS)
        if (kind == 1 && https) np_https_release(https);
#endif
#if !defined(__vita__)
        if (kind == 2 && plain) {
            int expect = 0;
            if (!plain->state.compare_exchange_strong(expect, 2)) delete plain;   // finished: ours to free
        }
        plain = 0;
#endif
        kind = 0;
        https = 0;
    }
};

// Starts method url+path. Returns false (and nothing is pending) when it cannot start.
bool StartRequest(Req* r, const char* method, const std::string& path, const std::string& body, bool auth, uint32_t timeoutMs = 15000) {
    const std::string base = ServiceUrl();
    if (base.empty()) return false;
    if (path != "/v1/info" && BusyLeftMs() > 0) { mp_log("service call %s held back: the service said it is busy (%u ms left)", path.c_str(), unsigned(BusyLeftMs())); return false; }
    const std::string url = base + path;
    std::string hAuth = std::string("Authorization: Bearer ") + g_acct.token;
    const std::string hBuild = std::string("X-Msc-Build: ") + g_build;
    const char* hdr[6] = {"X-Msc-Game: smg", "X-Msc-Proto: 1", "X-Msc-App: " SMG_APP_VERSION, 0, 0, 0};
    int nh = 3;
    if (auth) hdr[nh++] = hAuth.c_str();
    if (g_build[0]) hdr[nh++] = hBuild.c_str();
    if (url.compare(0, 8, "https://") == 0) {
#if defined(MP_SVC_HAVE_HTTPS)
        NpHttpsRequest rq;
        memset(&rq, 0, sizeof rq);
        rq.url = url.c_str();
        rq.method = method;
        rq.body = body.empty() ? 0 : body.data();
        rq.body_len = body.size();
        rq.headers = hdr;
        rq.total_timeout_ms = timeoutMs;
        rq.max_response = 32 * 1024;
        rq.keep_alive = 1;
        NpHttpsError e = NP_HTTPS_OK;
        const uint32_t h = np_https_start(&rq, &e);
        hAuth.assign(hAuth.size(), 0);
        if (!h) { mp_log("service request refused path=%s err=%s", path.c_str(), np_https_error_name(e)); return false; }
        r->kind = 1;
        r->https = h;
        return true;
#else
        return false;
#endif
    }
#if !defined(__vita__)
    // http://host[:port]
    std::string rest = url.substr(7);
    const size_t slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    std::string p = slash == std::string::npos ? "/" : rest.substr(slash);
    Plain* q = new Plain();
    if (timeoutMs != 15000) q->timeoutMs = timeoutMs;   // (conntest: the info call's own, shorter timeout)
    if (const char* t = getenv("SMG_ONLINE_TIMEOUT_MS")) { const long v = atol(t); if (v >= 50 && v <= 60000) q->timeoutMs = uint32_t(v); }   // (tests: a short timeout)
    const size_t colon = hostport.find(':');
    q->host = hostport.substr(0, colon);
    q->port = colon == std::string::npos ? 80 : uint16_t(atoi(hostport.c_str() + colon + 1));
    std::string rqText = std::string(method) + " " + p + " HTTP/1.1\r\nHost: " + hostport + "\r\nUser-Agent: GalaxyVita\r\nConnection: close\r\nAccept-Encoding: identity\r\n";   // (the same User-Agent as np_https: a request without one can be refused before the Worker runs)
    for (int i = 0; hdr[i]; ++i) rqText += std::string(hdr[i]) + "\r\n";
    if (!body.empty() || strcmp(method, "POST") == 0) {
        char cl[64];
        snprintf(cl, sizeof cl, "Content-Type: application/json\r\nContent-Length: %u\r\n", (unsigned)body.size());   // (%u: the Vita libc has no %zu)
        rqText += cl;
    }
    rqText += "\r\n" + body;
    q->request.swap(rqText);
    hAuth.assign(hAuth.size(), 0);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    const int rc = pthread_create(&t, &at, PlainThread, q);
    pthread_attr_destroy(&at);
    if (rc != 0) { delete q; return false; }
    r->kind = 2;
    r->plain = q;
    return true;
#else
    return false;
#endif
}

// 0 pending, 1 done (*out filled; the slot is free again), -1 nothing pending.
int PollRequest(Req* r, Answer* out) {
    if (r->kind == 1) {
#if defined(MP_SVC_HAVE_HTTPS)
        NpHttpsResult res;
        memset(&res, 0, sizeof res);
        const int rc = np_https_poll(r->https, &res);
        if (rc == 0) return 0;
        if (rc < 0) { out->transport = 2; out->status = 0; out->body.clear(); r->Drop(); return 1; }
        if (res.error == NP_HTTPS_OK || res.error == NP_HTTPS_E_HTTP_STATUS) {
            out->transport = 0;
            out->status = res.http_status;
            out->body.assign(reinterpret_cast<const char*>(res.body), res.body_len);
            if (!mp_svc::IsBusyAnswer(0, out->status, out->body.data(), out->body.size()) && out->status < 500) BusyClear();   // quota32: the service answered normally
            if (mp_update_scan_body) mp_update_scan_body(out->body.data(), out->body.size());   // release3: "a newer version is available" (once per boot, port/mp_update.h)
        } else {
            // service review: a clock years off fails the certificate dates: say so, so the menu can ask for the date and time
            const bool clock = res.error == NP_HTTPS_E_CLOCK ||
                               (res.error == NP_HTTPS_E_CERT && (res.cert_flags & (NP_HTTPS_CERT_EXPIRED | NP_HTTPS_CERT_NOT_YET_VALID)));
            out->transport = clock ? 3 : res.error == NP_HTTPS_E_OFFLINE ? 1 : 2;
            out->status = res.http_status;
            out->body.clear();
            mp_log("service transport error=%s stage=%s", np_https_error_name(res.error), np_https_stage_name(res.stage));
        }
        r->Drop();
        return 1;
#endif
    }
#if !defined(__vita__)
    if (r->kind == 2) {
        if (r->plain->state.load() != 1) return 0;
        *out = r->plain->res;
        if (out->transport == 0 && out->status < 500 && !mp_svc::IsBusyAnswer(0, out->status, out->body.data(), out->body.size())) BusyClear();   // quota32
        if (out->transport == 0 && mp_update_scan_body) mp_update_scan_body(out->body.data(), out->body.size());   // release3
        delete r->plain;
        r->plain = 0;
        r->kind = 0;
        return 1;
    }
#endif
    return -1;
}

// ---- results and errors --------------------------------------------------------------------------------------------------------
MpUiError g_lastError = MP_ERR_NONE;
char g_lastCode[32] = {};


// Turns a failed answer into an MpUiError (and remembers the service's own code). Returns the error.
MpUiError Fail(const char* op, const Answer& a) {
    std::string code;
    if (a.transport == 0) {
        NpNet::Json j;
        if (j.Parse(a.body.data(), a.body.size())) code = j.GetStr(j.Get(j.Root(), "error"), "code");
    }
    MpUiError e = MP_ERR_INTERNAL;
    if (a.transport == 3) code = "clock";
    if (a.transport == 3) e = MP_ERR_CLOCK;   // the menu: "Please set the date and time in the system settings."
    else if (a.transport == 1) e = MP_ERR_NO_WIFI;
    else if (a.transport) e = MP_ERR_SERVICE_DOWN;
    else if (a.status == 426) e = MP_ERR_UPDATE_REQUIRED;
    else if (mp_svc::IsBusyAnswer(a.transport, a.status, a.body.data(), a.body.size())) {
        // quota32: Cloudflare's own page (the daily free limit, a rate rule) or the service's own over_budget: not "too many tries" and not
        // "unavailable": the game says "online play is busy right now" and stops asking for a while (BusyNote)
        e = MP_ERR_SERVICE_DOWN;
        code = "service_busy";
        BusyNote();
    }
    else if (code == "game_mismatch" || code == "version_mismatch") e = MP_ERR_DIFFERENT_VERSION;
    else if (code == "name_taken") e = MP_ERR_NAME_TAKEN;
    else if (code == "bad_name") e = MP_ERR_BAD_NAME;
    else if (a.status == 429) e = MP_ERR_RATE_LIMITED;   // (the service's own envelope: rate_limited)
    else if (a.status >= 500 || a.status == 401) e = MP_ERR_SERVICE_DOWN;
    else if (a.status < 400) e = MP_ERR_SERVICE_DOWN;   // a 2xx that is not our JSON (captive portal, proxy page) or a redirect: not our service
    else if (code == "room_not_found" || code == "room_closed" || code == "room_expired") e = MP_ERR_ROOM_NOT_FOUND;
    else if (code == "room_full" || code == "room_started") e = MP_ERR_ROOM_FULL;
    if (a.transport == 0 && a.status == 401) {
        g_acct.registered = false;   // the service no longer knows this account: offer to register again (the file stays)
        TellCoreCode("");
        mp_log("service account refused by the service: register again");
    }
    g_lastError = e;
    CopyStr(g_lastCode, sizeof g_lastCode, code);
    mp_log("service error op=%s status=%d code=%s", op, a.transport ? -a.transport : a.status, code.empty() ? "-" : code.c_str());
    return e;
}

void FailLocal(MpUiError e, const char* code) {
    g_lastError = e;
    CopyStr(g_lastCode, sizeof g_lastCode, code);
}
// A call that could not start: "service_busy" while the busy gate is closed (the menus say "online play is busy"), else "start".
void FailStart() { FailLocal(MP_ERR_SERVICE_DOWN, BusyLeftMs() > 0 ? "service_busy" : "start"); }

// ---- operations ----------------------------------------------------------------------------------------------------------------
enum { kMaxFriends = 50, kMaxBoard = 50, kSubmitQueue = 4 };

struct Op {
    Req req;
    int state = 0;   // 0 none, -1 running, 1 done, -2 failed
};
Op g_register, g_friends, g_friendOp, g_board, g_submit;
Op g_acctDelete;   // Delete My Online Account: DELETE /v1/me (svcfix: no body; 200 {"ok":true}; 401 = the service no longer has it)
char g_pendingName[MP_NAME_BYTES + 1] = {};
bool g_refreshFriends = false;

MpFriend g_friendRows[kMaxFriends];
int g_friendCount = 0;
MpFriend g_incomingRows[kMaxFriends];   // MP3: requests from players who added us (dismiss or block them); name + code only
int g_incomingCount = 0;
MpBoardRow g_boardRows[kMaxBoard];
int g_boardCount = 0;

struct Submit {
    bool used = false;
    uint8_t galaxy = 0, star = 0, players = 1, tries = 0;
    uint32_t frames = 0;
    uint64_t notBeforeMs = 0;
    uint64_t queuedAt = 0;   // quota32: which waiting time gives way when the queue is full
    char room[8] = {};
    char digest[33] = {};
};
Submit g_queue[kSubmitQueue];
int g_inflight = -1;   // queue index of the running submission

bool Ready(const char* op) {
    // fix3: every request starts with a clean slate, so the menus never word an older failure (mp_service_last_code)
    g_lastError = MP_ERR_NONE;
    g_lastCode[0] = 0;
    LoadAccount();
    if (ServiceUrl().empty()) { FailLocal(MP_ERR_SERVICE_DOWN, "offline_build"); mp_log("service off op=%s (no service URL)", op); return false; }
    return true;
}

bool NeedAccount(const char* op) {
    if (!Ready(op)) return false;
    if (!g_acct.registered) { FailLocal(MP_ERR_INTERNAL, "not_registered"); mp_log("service op=%s needs an account", op); return false; }
    return true;
}

// ---- sign-up without a sign-up screen (service review, owner: "as simple as possible") ------------------------------------------
// mp_service_ensure_account() and mp_ui_register() share one registration: GET /v1/register/challenge, the proof of work solved in
// slices from mp_service_pump() (mp_pow.h), POST /v1/register. A refused name, a taken name (an older service) or a stale challenge is
// handled here without the player: the name falls back to "Player", a taken one gets two digits, a challenge is fetched again (at most
// kRegRestarts times). Every other refusal ends the attempt with one of four reasons (mp_service_last_code): "busy", "no_wifi",
// "clock", "update"; the next attempt waits (10 s doubling to 5 min, with jitter) however often the menu asks.
enum EnsureStage { ES_IDLE, ES_CHALLENGE, ES_SOLVING, ES_REGISTER, ES_FAILED };
EnsureStage g_es = ES_IDLE;
Op g_challenge, g_rename;
mp_pow::Solver g_solver;
std::string g_powChallenge;
char g_regName[MP_NAME_BYTES + 1] = {};
int g_regRestarts = 0;
const int kRegRestarts = 3;
int g_ensureFails = 0;
uint64_t g_ensureRetryAt = 0;
MpUiError g_ensureErr = MP_ERR_NONE;
char g_ensureCode[16] = {};
int g_reregs = 0;                  // quiet registrations after the service forgot the account, this boot (at most kMaxReregs)
const int kMaxReregs = 2;
char g_nameOverride[MP_NAME_BYTES + 1] = {};   // mp_service_set_name (the profile's name); else mp_config()->name
char g_renameTried[MP_NAME_BYTES + 1] = {};
uint64_t g_solveWindowAt = 0;
uint32_t g_solveUsedMs = 0;
// at most this much solving per 16 ms window (whatever the number of pump calls in a frame): a menu frame keeps its pace
const uint32_t kSolveBudgetMs = 6, kSolveWindowMs = 16;
uint32_t Jitter(uint32_t base);

// The name shown when the player's own is unusable or refused: the character's (never "Player", never an error).
void CharacterName(char out[MP_NAME_BYTES + 1]) {
    out[0] = 0;
    int look = 0;
    if (mp_config) if (const MpConfig* c = mp_config()) look = c->look;
    static const char* const kChar[MP_LOOK_COUNT] = {"Mario", "Luigi", "Toad", "Cosmic Mario", "Yellow Toad", "Cosmic Luigi"};   // (as mp_name_or_character)
    snprintf(out, 11, "%s", kChar[look >= 0 && look < MP_LOOK_COUNT ? look : 0]);   // (at most 10 characters: "Cosmic Mario" is cut)
}

// The service's name rules from any profile name: keep [A-Za-z0-9 ._-], no double spaces, letters or digits at both ends, 10 at
// most. A name with nothing usable left (a Japanese console name, for one) is the character's name: names are display names, nobody is refused.
void ServiceName(char out[MP_NAME_BYTES + 1]) {
    const char* src = g_nameOverride[0] ? g_nameOverride : "";
    if (!*src && mp_config) if (const MpConfig* c = mp_config()) src = c->name;
    char buf[64];
    size_t n = 0;
    for (const char* p = src; *p && n < sizeof buf - 1; ++p) {
        const char ch = *p;
        if (!(IsAlnum(ch) || ch == ' ' || ch == '.' || ch == '_' || ch == '-')) continue;
        if (ch == ' ' && (n == 0 || buf[n - 1] == ' ')) continue;
        buf[n++] = ch;
    }
    buf[n] = 0;
    size_t start = 0;
    while (buf[start] && !IsAlnum(buf[start])) ++start;
    size_t len = strlen(buf + start);
    if (len > 10) len = 10;
    while (len && !IsAlnum(buf[start + len - 1])) --len;
    char cand[16];
    memcpy(cand, buf + start, len);
    cand[len] = 0;
    char clean[MP_NAME_BYTES + 1];
    if (NameValid(cand, clean, sizeof clean) && mp_nf::Reason(clean, strlen(clean)) == mp_nf::OK) snprintf(out, MP_NAME_BYTES + 1, "%s", clean);
    else CharacterName(out);
}

const char* ReasonOf(const Answer& a) {
    if (a.transport == 3) return "clock";
    if (a.transport == 1) return "no_wifi";
    if (a.transport == 0 && a.status == 426) return "update";
    return "busy";   // server_full, the daily caps, rate limits, service_disabled, 5xx, unreachable, a reply that is not ours
}
MpUiError ErrOfReason(const char* r) {
    if (!strcmp(r, "no_wifi")) return MP_ERR_NO_WIFI;
    if (!strcmp(r, "update")) return MP_ERR_UPDATE_REQUIRED;
    if (!strcmp(r, "clock")) return MP_ERR_CLOCK;
    return MP_ERR_SERVICE_DOWN;
}

void RegFailReason(const char* reason) {
    g_register.state = -2;
    g_es = ES_FAILED;
    ++g_ensureFails;
    const int sh = g_ensureFails - 1 < 5 ? g_ensureFails - 1 : 5;
    uint32_t wait = 10000u << sh;
    if (wait > 300000u) wait = 300000u;
    g_ensureRetryAt = NowMs() + Jitter(wait);
    snprintf(g_ensureCode, sizeof g_ensureCode, "%s", reason);
    g_ensureErr = ErrOfReason(reason);
    g_lastError = g_ensureErr;
    CopyStr(g_lastCode, sizeof g_lastCode, reason);
    mp_log("service sign-up failed reason=%s (next try in %u ms)", reason, unsigned(g_ensureRetryAt - NowMs()));
}
void RegFail(const char* op, const Answer& a) {
    Fail(op, a);
    const bool busy = mp_svc::IsBusyAnswer(a.transport, a.status, a.body.data(), a.body.size());
    RegFailReason(ReasonOf(a));
    if (busy) {   // quota32: a quota page: not before the busy gate opens (60 s, doubling)
        const uint64_t floorAt = NowMs() + BusyLeftMs();
        if (g_ensureRetryAt < floorAt) g_ensureRetryAt = floorAt;
    }
}

void StartChallenge() {
    if (!StartRequest(&g_challenge.req, "GET", "/v1/register/challenge", std::string(), false)) { RegFailReason("busy"); return; }
    g_challenge.state = -1;
    g_es = ES_CHALLENGE;
}

void BeginRegistration(const char* name) {
    snprintf(g_regName, sizeof g_regName, "%s", name);
    snprintf(g_pendingName, sizeof g_pendingName, "%s", name);
    g_regRestarts = 0;
    g_register.state = -1;
    mp_log("service sign-up start name=%s", name);
    StartChallenge();
}

void PostRegister(bool withPow) {
    std::string body = "{\"name\":";
    NpNet::JsonEscapeAppend(&body, g_regName);
    if (withPow) {
        body += ",\"pow\":{\"challenge\":";
        NpNet::JsonEscapeAppend(&body, g_powChallenge.c_str());
        body += ",\"nonce\":";
        NpNet::JsonEscapeAppend(&body, g_solver.nonce);
        body += "}";
    }
    body += "}";
    if (!StartRequest(&g_register.req, "POST", "/v1/register", body, false)) { RegFailReason("busy"); return; }
    g_es = ES_REGISTER;
}

// a challenge string as the service makes them: digits, dots and hex, 20..80 characters (anything else is not ours)
bool ChallengeValid(const std::string& c) {
    if (c.size() < 20 || c.size() > 80) return false;
    for (char ch : c) if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || ch == '.')) return false;
    return true;
}

void OnChallenge(const Answer& a) {
    g_challenge.state = 0;
    NpNet::Json j;
    if (a.transport == 0 && a.status == 200 && j.Parse(a.body.data(), a.body.size()) && j.TypeOf(j.Root()) == NpNet::Json::OBJ) {
        const double bits = j.GetNum(j.Root(), "bits", -1);
        if (bits == 0) { PostRegister(false); return; }
        const std::string& ch = j.GetStr(j.Root(), "challenge");
        // (svcfix2: more than mp_pow::kMaxBits (17) is more than about 10 s of grinding on a Vita: not a difficulty this client spends its time on.
        // The player sees the kind "online play is busy" line, and the next attempt waits (10 s doubling) like any other busy answer.)
        if (bits > mp_pow::kMaxBits) { mp_log("service sign-up: proof of work bits=%d is above this client's limit (%d)", int(bits), mp_pow::kMaxBits); RegFailReason("busy"); return; }
        if (bits >= 1 && ChallengeValid(ch) && g_solver.Start(ch.c_str(), int(bits), 1ull << 27)) {
            g_powChallenge = ch;
            g_es = ES_SOLVING;
            mp_log("service sign-up: proof of work bits=%d", int(bits));
            return;
        }
        Answer bad = a;
        bad.transport = 2;   // (an answer that is not usable: not an edge page either, see mp_svc_busy.h)
        RegFail("challenge", bad);
        return;
    }
    // an older service without the route: register without a proof (it asks for one with 428 if it wants one after all)
    if (a.transport == 0 && a.status == 404 && g_regRestarts == 0) { PostRegister(false); return; }
    RegFail("challenge", a);
}

void SolveSlice() {
    if (g_es != ES_SOLVING) return;
    uint64_t now = NowMs();
    if (now - g_solveWindowAt >= kSolveWindowMs || now < g_solveWindowAt) { g_solveWindowAt = now; g_solveUsedMs = 0; }
    if (g_solveUsedMs >= kSolveBudgetMs) return;
    const uint64_t start = now;
    int r = 0;
    while (r == 0) {
        r = g_solver.Step(256);
        now = NowMs();
        if (now - start >= kSolveBudgetMs - g_solveUsedMs) break;
    }
    g_solveUsedMs += uint32_t(now - start);
    if (r == 1) PostRegister(true);
    else if (r < 0) RegFailReason("busy");
}

void OnRegister(const Answer& a) {
    NpNet::Json j;
    const bool parsed = a.transport == 0 && j.Parse(a.body.data(), a.body.size());
    if (parsed && a.status == 200) {
        const int root = j.Root();
        const double pidNum = j.GetNum(root, "pid", 0);
        const int pid = (pidNum >= 1 && pidNum <= 2147483647.0) ? int(pidNum) : 0;
        const std::string& token = j.GetStr(root, "token");
        const std::string& code = j.GetStr(root, "code");
        const std::string& name = j.GetStr(root, "name");
        char cleanCode[10], cleanName[MP_NAME_BYTES + 1];
        if (pid > 0 && token.size() < kTokenBytes && TokenValid(token.c_str()) && FriendCodeValid(code.c_str(), cleanCode) &&
            NameValid(name.c_str(), cleanName, sizeof cleanName)) {
            if (!SaveAccount(pid, token.c_str(), cleanName, cleanCode)) mp_log("service account not saved (disk)");
            g_acct.registered = true;
            g_acct.pid = pid;
            snprintf(g_acct.token, sizeof g_acct.token, "%s", token.c_str());
            snprintf(g_acct.name, sizeof g_acct.name, "%s", cleanName);
            memcpy(g_acct.code, cleanCode, sizeof g_acct.code);
            TellCoreCode(g_acct.code);
            g_register.state = 1;
            g_es = ES_IDLE;
            g_ensureFails = 0;
            g_hadAccount = true;
            snprintf(g_renameTried, sizeof g_renameTried, "%s", g_regName);   // (the name it was made with: no rename for it)
            g_lastError = MP_ERR_NONE;
            g_lastCode[0] = 0;
            mp_log("service register ok code=%s", g_acct.code);
            return;
        }
        Answer bad = a;
        bad.transport = 2;
        RegFail("register", bad);
        return;
    }
    std::string code;
    if (parsed && a.status >= 400) code = j.GetStr(j.Get(j.Root(), "error"), "code");
    if (g_regRestarts < kRegRestarts && (code == "bad_pow" || code == "pow_required" || code == "name_taken" || code == "bad_name")) {
        ++g_regRestarts;
        if (code == "bad_name") CharacterName(g_regName);
        else if (code == "name_taken") {
            // (an older service with unique names): two digits after at most 8 characters of the name
            char base[MP_NAME_BYTES + 1];
            snprintf(base, sizeof base, "%s", g_regName);
            size_t n = strlen(base);
            if (n > 8) n = 8;
            while (n && !IsAlnum(base[n - 1])) --n;
            base[n] = 0;
            snprintf(g_regName, sizeof g_regName, "%s%02u", base, unsigned(Jitter(4000) % 100));
        }
        mp_log("service sign-up: %s, trying again (name=%s)", code.c_str(), g_regName);
        StartChallenge();
        return;
    }
    RegFail("register", a);
}

void OnRename(const Answer& a) {
    g_rename.state = 0;
    NpNet::Json j;
    if (a.transport == 0 && a.status == 200 && j.Parse(a.body.data(), a.body.size())) {
        char clean[MP_NAME_BYTES + 1];
        if (NameValid(j.GetStr(j.Root(), "name").c_str(), clean, sizeof clean)) {
            snprintf(g_acct.name, sizeof g_acct.name, "%s", clean);
            SaveAccount(g_acct.pid, g_acct.token, g_acct.name, g_acct.code);
            mp_log("service name now %s", g_acct.name);
            return;
        }
    }
    // a refused name keeps the old one online; nothing for the player to do (and no retry until the name changes again)
    mp_log("service rename refused status=%d: the old name stays", a.transport ? -a.transport : a.status);
    if (a.transport == 0 && a.status == 401) {
        g_acct.registered = false;
        TellCoreCode("");
    }
}

void OnFriends(const Answer& a) {
    NpNet::Json j;
    if (a.transport == 0 && a.status == 200 && j.Parse(a.body.data(), a.body.size())) {
        const int list = j.Get(j.Root(), "friends");
        int n = 0;
        for (int i = 0; i < j.Size(list) && n < kMaxFriends; ++i) {
            const int f = j.At(list, i);
            MpFriend& row = g_friendRows[n];
            row = MpFriend();
            char clean[MP_NAME_BYTES + 1];
            if (!NameValid(j.GetStr(f, "name").c_str(), clean, sizeof clean) || !FriendCodeValid(j.GetStr(f, "code").c_str(), row.code)) continue;
            snprintf(row.name, sizeof row.name, "%s", clean);
            row.online = j.GetBool(f, "online") && j.GetBool(f, "mutual");
            const int room = j.Get(f, "room");
            row.joinable = j.TypeOf(room) == NpNet::Json::OBJ && j.GetBool(room, "joinable");
            const std::string& mode = j.GetStr(room, "mode");
            row.mode = mode == "coop" ? MP_MODE_TOGETHER : mode == "race" ? MP_MODE_RACE : MP_MODE_NONE;
            ++n;
        }
        g_friendCount = n;
        {   // lanfix: friends' names outrank strangers in the Same Wi-Fi room list (mp_lan.h)
            char names[kMaxFriends][MP_NAME_BYTES + 1];
            for (int i = 0; i < n; ++i) memcpy(names[i], g_friendRows[i].name, sizeof names[i]);
            if (mp_lan_set_trusted_names) mp_lan_set_trusted_names(1, names, n);
        }
        // MP3: "incoming" = players who added us and are not our friends yet (rows keep online/joinable false)
        const int inc = j.Get(j.Root(), "incoming");
        int m = 0;
        for (int i = 0; i < j.Size(inc) && m < kMaxFriends; ++i) {
            const int f = j.At(inc, i);
            MpFriend& row = g_incomingRows[m];
            row = MpFriend();
            char clean[MP_NAME_BYTES + 1];
            if (!NameValid(j.GetStr(f, "name").c_str(), clean, sizeof clean) || !FriendCodeValid(j.GetStr(f, "code").c_str(), row.code)) continue;
            snprintf(row.name, sizeof row.name, "%s", clean);
            ++m;
        }
        g_incomingCount = m;
        g_friends.state = 1;
        int online = 0;
        for (int i = 0; i < n; ++i) online += g_friendRows[i].online ? 1 : 0;
        mp_log("service friends n=%d online=%d incoming=%d", n, online, m);
        return;
    }
    Fail("friends", a);
    g_friends.state = -2;
}

void OnFriendOp(const Answer& a) {
    NpNet::Json j;
    // (a 200 that is not the service's {"ok":true}: a captive portal or proxy page, not a friend added)
    if (a.transport == 0 && a.status == 200 && j.Parse(a.body.data(), a.body.size()) && j.GetBool(j.Root(), "ok")) {
        g_friendOp.state = 1;
        mp_log("service friend op ok");
        g_refreshFriends = true;   // the list changed: fetch it again
        return;
    }
    Fail("friend", a);
    g_friendOp.state = -2;
}

// The service has removed the account (or never had it): this console forgets it too. Only the account's own file (online.dat or this address's
// online_<hex>.dat, with the leftovers of its last write: .mpbak / .mptmp, which a boot would otherwise put back) and the cached friend lists go;
// older credentials kept as .prev and every other file stay.
void ForgetAccountHere() {
    const std::string path = AccountPath(g_acctUrl.empty() ? ServiceUrl() : g_acctUrl);
    // (the copies first, the account file last: a power cut half way must never leave a backup that restores a token the service already deleted)
    const std::string names[3] = {path + ".mptmp", path + ".mpbak", path};
    for (const std::string& n : names) {
        if (unlink(n.c_str()) != 0 && errno != ENOENT) mp_log("service account file not removed path=%s errno=%d", n.c_str(), errno);
    }
    memset(g_acct.token, 0, sizeof g_acct.token);
    g_acct = Account();
    g_acct.loaded = true;   // (loaded: no account, not a file to read again)
    g_hadAccount = false;   // (the next sign-up is a first one, not a re-registration)
    g_netConfigured = false;
    TellCoreCode("");
    g_friends.req.Drop();
    g_friends.state = 0;
    g_friendCount = g_incomingCount = 0;
    g_refreshFriends = false;
}

void OnAcctDelete(const Answer& a) {
    NpNet::Json j;
    const bool said = a.transport == 0 && a.status == 200 && j.Parse(a.body.data(), a.body.size()) && j.GetBool(j.Root(), "ok");
    const bool gone = a.transport == 0 && a.status == 401;   // (the service does not know the token: the account is already gone there)
    if (said || gone) {
        ForgetAccountHere();
        g_acctDelete.state = 1;
        mp_log("service account deleted%s", gone ? " (it was already gone)" : "");
        return;
    }
    const bool wasRegistered = g_acct.registered;
    Fail("account_delete", a);
    g_acct.registered = wasRegistered;   // (a failed delete changes nothing here, whatever the answer was)
    g_acctDelete.state = -2;
}

void OnBoard(const Answer& a) {
    NpNet::Json j;
    if (a.transport == 0 && a.status == 200 && j.Parse(a.body.data(), a.body.size())) {
        const int list = j.Get(j.Root(), "rows");
        int n = 0;
        for (int i = 0; i < j.Size(list) && n < kMaxBoard; ++i) {
            const int r = j.At(list, i);
            const double rank = j.GetNum(r, "rank", 0), frames = j.GetNum(r, "frames", -1), players = j.GetNum(r, "players", 0);
            char clean[MP_NAME_BYTES + 1];
            if (!(rank >= 1 && rank < 1e9 && frames >= 0 && frames < 4e9 && players >= 1 && players <= 4)) continue;
            if (!NameValid(j.GetStr(r, "name").c_str(), clean, sizeof clean)) continue;
            MpBoardRow& row = g_boardRows[n++];
            row = MpBoardRow();
            row.rank = uint32_t(rank);
            snprintf(row.name, sizeof row.name, "%s", clean);
            row.frames = uint32_t(frames);
            row.players = uint8_t(players);
            row.verified = j.GetBool(r, "verified");
            row.you = j.GetBool(r, "you");
        }
        g_boardCount = n;
        g_board.state = 1;
        mp_log("service board rows=%d total=%d", n, int(j.GetNum(j.Root(), "total", 0)));
        return;
    }
    Fail("board", a);
    g_board.state = -2;
}

// A retry delay spread over -25 % .. +25 % of `base`, so consoles that failed together (the service was down for everybody) do not
// come back together. xorshift32, seeded on first use from the clock and a stack address: nothing here needs to be unpredictable.
uint32_t Jitter(uint32_t base) {
    static uint32_t x = 0;
    if (!x) {
        int probe = 0;
        x = uint32_t(NowMs()) * 2654435761u ^ uint32_t(reinterpret_cast<uintptr_t>(&probe)) ^ 0x9E3779B9u;
        if (!x) x = 1;
    }
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    const uint32_t span = base / 2;   // 50 % wide
    return base - base / 4 + (span ? x % (span + 1) : 0);
}

void OnSubmit(const Answer& a) {
    Submit& s = g_queue[g_inflight];
    NpNet::Json j;
    if (a.transport == 0 && a.status == 200 && j.Parse(a.body.data(), a.body.size())) {
        const int root = j.Root();
        mp_log("service time submit galaxy=%u star=%u frames=%u best=%d verified=%d rank=%d", unsigned(s.galaxy), unsigned(s.star),
               unsigned(s.frames), j.GetBool(root, "best") ? 1 : 0, j.GetBool(root, "verified") ? 1 : 0, int(j.GetNum(root, "rank", 0)));
        s.used = false;
    } else {
        Fail("time", a);
        // quota32: a time is never dropped because the service is busy, down or unreachable (a 429 / 5xx, Cloudflare's quota page, no Wi-Fi, a
        // timeout): it waits in the queue and is sent again later, 5 s, 10 s, then 30 s doubling to 10 minutes, and not before the busy gate
        // opens. Only the service's own refusal of the time itself (a 4xx with its code) ends it, as before.
        const bool transient = a.transport != 0 || a.status >= 500 || a.status == 429;
        if (transient) {
            if (s.tries < 250) ++s.tries;
            uint32_t wait = s.tries <= 2 ? 5000u * s.tries : 30000u << (s.tries - 3 < 5 ? s.tries - 3 : 5);
            if (a.transport == 0 && a.status == 429 && wait < 60000u && !mp_svc::IsBusyAnswer(0, a.status, a.body.data(), a.body.size())) wait = 60000u;   // (the service's own rate limit; a busy answer waits for the busy gate)
            if (wait > 600000u) wait = 600000u;
            s.notBeforeMs = NowMs() + Jitter(wait);
            const uint64_t gate = NowMs() + BusyLeftMs();
            if (BusyLeftMs() > 0 && s.notBeforeMs < gate) s.notBeforeMs = gate;
            mp_log("service time submit kept for later (try %u, in %u ms)", unsigned(s.tries), unsigned(s.notBeforeMs - NowMs()));
        }
        else if (a.transport == 0 && a.status == 403 && s.room[0] && !strcmp(g_lastCode, "not_member")) {
            // fix3: the service dropped this console from the room meanwhile (a minute of silence): the time still counts, without
            // the room's verification; sent again once without the room
            mp_log("service time submit: not in room %s any more: sent again without it", s.room);
            s.room[0] = 0;
            s.notBeforeMs = 0;
        } else s.used = false;
    }
    g_inflight = -1;
}

std::string SubmitBody(const Submit& s) {
    char b[320];
    int n = snprintf(b, sizeof b, "{\"galaxy\":%u,\"star\":%u,\"frames\":%u,\"players\":%u,\"build\":\"%s\"", unsigned(s.galaxy), unsigned(s.star),
                     unsigned(s.frames), unsigned(s.players), SMG_TIME_BUILD);
    if (s.room[0] && n > 0 && n < int(sizeof b)) n += snprintf(b + n, sizeof b - size_t(n), ",\"room\":\"%s\"", s.room);
    if (s.digest[0] && n > 0 && n < int(sizeof b)) n += snprintf(b + n, sizeof b - size_t(n), ",\"digest\":\"%s\"", s.digest);
    if (n > 0 && n < int(sizeof b) - 1) { b[n++] = '}'; b[n] = 0; }
    return std::string(b);
}

// ---- room lookups for MpCore (quick match, a friend's room) ------------------------------------------------------------------------
Op g_lookup;
int g_lookupKind = 0;              // 0 quick match (public rooms of the mode), 1 a friend's room
char g_lookupFriend[10] = {};
char g_lookupCode[16] = {};
MpUiError g_lookupError = MP_ERR_NONE;

bool RoomCodeValid(const std::string& c) {
    if (c.size() < 4 || c.size() > 12) return false;
    for (char ch : c) if (!IsAlnum(ch)) return false;
    return true;
}

void OnLookup(const Answer& a) {
    NpNet::Json j;
    g_lookupCode[0] = 0;
    if (a.transport == 0 && a.status == 200 && j.Parse(a.body.data(), a.body.size())) {
        if (g_lookupKind == 0) {
            const int list = j.Get(j.Root(), "rooms");
            for (int i = 0; i < j.Size(list) && i < 20; ++i) {
                const int r = j.At(list, i);
                const std::string& code = j.GetStr(r, "code");
                const double players = j.GetNum(r, "players", 0), maxM = j.GetNum(r, "maxMembers", 4);
                if (!RoomCodeValid(code) || !(players >= 1 && players < maxM && maxM <= 4)) continue;
                CopyStr(g_lookupCode, sizeof g_lookupCode, code);
                break;
            }
            mp_log("service quick match lookup room=%s", g_lookupCode[0] ? g_lookupCode : "-");
            g_lookup.state = 1;
            return;
        }
        const int list = j.Get(j.Root(), "friends");
        for (int i = 0; i < j.Size(list) && i < kMaxFriends; ++i) {
            const int f = j.At(list, i);
            char code[10];
            if (!FriendCodeValid(j.GetStr(f, "code").c_str(), code) || strcmp(code, g_lookupFriend) != 0) continue;
            const int room = j.Get(f, "room");
            const std::string& rc = j.GetStr(room, "code");
            if (j.TypeOf(room) == NpNet::Json::OBJ && j.GetBool(room, "joinable") && RoomCodeValid(rc)) {
                CopyStr(g_lookupCode, sizeof g_lookupCode, rc);
                mp_log("service friend lookup friend=%s room=%s", g_lookupFriend, g_lookupCode);
                g_lookup.state = 1;
                return;
            }
            break;
        }
        mp_log("service friend lookup friend=%s: no joinable room", g_lookupFriend);
        g_lookupError = MP_ERR_ROOM_NOT_FOUND;
        g_lookup.state = -2;
        return;
    }
    g_lookupError = Fail("lookup", a);
    g_lookup.state = -2;
}

void PumpOne(Op* op, void (*done)(const Answer&)) {
    if (op->req.kind == 0) return;
    Answer a{2, 0, std::string()};
    if (PollRequest(&op->req, &a) == 1) done(a);
}

// ---- code5: far-away invites (port/mp_invite.h; server/src/core/invites.js) ---------------------------------------------------------
// One register and one resolve at a time (each: wait for the account, then one request), a keep and a close that nobody waits for. The
// account is made quietly when there is none (mp_service_ensure_account), exactly as for every other online action.
struct Invite {
    Op op;
    int stage = 0;              // 0 idle, 1 waiting for the account, 2 request running, 3 done, 4 failed
    char code[MP_INVITE_LEN + 1] = {};
    std::string blob;           // register: what is sent; resolve: what came back
    int err = 0;                // MpInviteError when stage 4
    int keepSec = 300;
};
Invite g_invReg, g_invRes;
Op g_invKeep, g_invClose;

int InviteErrorOf(const Answer& a) {
    if (a.transport == 3) return MP_INV_CLOCK;
    if (a.transport == 1) return MP_INV_NO_WIFI;
    if (a.transport) return MP_INV_BUSY;
    std::string code;
    NpNet::Json j;
    if (j.Parse(a.body.data(), a.body.size())) code = j.GetStr(j.Get(j.Root(), "error"), "code");
    if (a.status == 426) return MP_INV_UPDATE;
    if (code == "invite_not_found" || code == "own_invite" || code == "bad_code") return MP_INV_WRONG;
    if (code == "invite_expired" || code == "invite_closed") return MP_INV_EXPIRED;
    if (code == "invite_full") return MP_INV_FULL;
    if (code == "version_mismatch" || code == "game_mismatch") return MP_INV_VERSION;
    if (code == "invite_taken") return MP_INV_TAKEN;
    // everything else is the service's trouble, not the player's code: over budget, switched off, too many tries, a captive portal, a 5xx
    return MP_INV_BUSY;
}

int InviteErrorOfEnsure() {
    const char* c = mp_service_last_code();
    if (!strcmp(c, "no_wifi")) return MP_INV_NO_WIFI;
    if (!strcmp(c, "clock")) return MP_INV_CLOCK;
    if (!strcmp(c, "update")) return MP_INV_UPDATE;
    if (!strcmp(c, "offline_build")) return MP_INV_OFF;
    return MP_INV_BUSY;
}

void InviteStart(Invite* v, bool resolve) {
    std::string body = "{\"code\":\"";
    body += v->code;
    body += "\"";
    if (!resolve) {
        body += ",\"blob\":\"";
        body += v->blob;
        body += "\"";
    }
    body += "}";
    if (!StartRequest(&v->op.req, "POST", resolve ? "/v1/invites/resolve" : "/v1/invites", body, true)) {
        v->err = MP_INV_BUSY;
        v->stage = 4;
        return;
    }
    v->stage = 2;
}

bool g_inviteInAccount = false;   // (mp_service_ensure_account pumps the service itself: the invites must not be pumped again inside their own account wait)
void InviteAccount(Invite* v, bool resolve) {   // stage 1: the account first (made quietly when there is none)
    if (g_inviteInAccount) return;
    g_inviteInAccount = true;
    const int e = mp_service_ensure_account();
    g_inviteInAccount = false;
    if (v->stage != 1) return;
    if (e == 0) return;
    if (e < 0) {
        v->err = InviteErrorOfEnsure();
        v->stage = 4;
        mp_log("service invite %s: no account (%s)", resolve ? "resolve" : "register", mp_service_last_code());
        return;
    }
    InviteStart(v, resolve);
}

void OnInviteAnswer(Invite* v, bool resolve, const Answer& a) {
    if (v->stage != 2) return;
    NpNet::Json j;
    if (a.transport == 0 && a.status == 200 && j.Parse(a.body.data(), a.body.size()) && j.TypeOf(j.Root()) == NpNet::Json::OBJ) {
        if (resolve) {
            const std::string& b = j.GetStr(j.Root(), "blob");
            if (b.size() == MP_INVITE_BLOB_CHARS) {
                v->blob = b;
                v->stage = 3;
                mp_log("service invite resolved");
                return;
            }
        } else {
            const int k = int(j.GetNum(j.Root(), "keepSec", 300));
            v->keepSec = k < 30 ? 30 : k > 1800 ? 1800 : k;
            v->stage = 3;
            mp_log("service invite registered (keep every %d s)", v->keepSec);
            return;
        }
    }
    Fail(resolve ? "invite_resolve" : "invite_register", a);
    v->err = a.transport == 0 && a.status == 200 ? MP_INV_BUSY : InviteErrorOf(a);   // (a 200 that is not our JSON: a captive portal)
    v->stage = 4;
}
void OnInvReg(const Answer& a) { OnInviteAnswer(&g_invReg, false, a); }
void OnInvRes(const Answer& a) { OnInviteAnswer(&g_invRes, true, a); }
void OnInvIgnored(const Answer& a) { if (a.transport || a.status >= 400) mp_log("service invite keep/close: %s %d", a.transport ? "transport" : "status", a.transport ? a.transport : a.status); }

void InvitePump() {
    if (g_inviteInAccount) return;
    if (g_invReg.stage == 1) InviteAccount(&g_invReg, false);
    if (g_invRes.stage == 1) InviteAccount(&g_invRes, true);
    PumpOne(&g_invReg.op, OnInvReg);
    PumpOne(&g_invRes.op, OnInvRes);
    PumpOne(&g_invKeep, OnInvIgnored);
    PumpOne(&g_invClose, OnInvIgnored);
}

bool InviteBegin(Invite* v, const char* code, const char* blob, bool resolve) {
    if (!Ready(resolve ? "invite_resolve" : "invite_register")) { v->err = ServiceUrl().empty() ? MP_INV_OFF : MP_INV_BUSY; v->stage = 4; return false; }
    char c[MP_INVITE_LEN + 1];
    if (!code || mp_invite_normalize(code, c) != MP_INV_TYPED_OK) { v->err = MP_INV_WRONG; v->stage = 4; return false; }
    if (!resolve && (!blob || strlen(blob) != MP_INVITE_BLOB_CHARS)) { v->err = MP_INV_BUSY; v->stage = 4; return false; }
    v->op.req.Drop();
    memcpy(v->code, c, sizeof c);
    v->blob = resolve ? std::string() : std::string(blob);
    v->err = MP_INV_NONE;
    v->stage = 1;
    InviteAccount(v, resolve);
    return true;
}

int InvitePoll(Invite* v, std::string* blobOut) {
    mp_service_pump();
    switch (v->stage) {
    case 1: case 2: return 0;
    case 3:
        if (blobOut) *blobOut = v->blob;
        v->stage = 0;
        return 1;
    case 4: {
        const int e = v->err ? v->err : MP_INV_BUSY;
        v->stage = 0;
        return -e;
    }
    default: return -MP_INV_BUSY;   // (nothing was started)
    }
}

bool StartFriends() {
    if (g_friends.state == -1) return true;
    if (!StartRequest(&g_friends.req, "GET", "/v1/friends", std::string(), true)) { FailStart(); g_friends.state = -2; return false; }
    g_friends.state = -1;
    return true;
}

// MP3 conntest: GET /v1/info for the Connection Test (reachability, switched-off, "needs a newer game"). One call at a time, polled by the
// caller (nothing else reads it), never authenticated. The answer is checked to be this service's: a 200 with a JSON object that has a
// numeric "proto"; anything else (a captive portal's page, a proxy's error) is "not our service".
Req g_infoReq;
uint64_t g_infoStartMs = 0;

bool VersionOlder(const char* mine, const char* minimum) {
    auto parse = [](const char* s, uint32_t out[4]) {
        int n = 0;
        if (!s || !*s) return 0;
        for (;;) {
            if (*s < '0' || *s > '9' || n >= 4) return 0;
            uint32_t v = 0;
            int digits = 0;
            while (*s >= '0' && *s <= '9') {
                if (++digits > 9) return 0;
                v = v * 10 + uint32_t(*s - '0');
                ++s;
            }
            out[n++] = v;
            if (!*s) return n;
            if (*s != '.') return 0;
            ++s;
        }
    };
    uint32_t a[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0};
    if (!parse(mine, a) || !parse(minimum, b)) return false;
    for (int i = 0; i < 4; i++)
        if (a[i] != b[i]) return a[i] < b[i];
    return false;
}

void ReadInfo(const Answer& a, MpServiceInfo* out) {
    out->transport = a.transport;
    out->status = a.status;
    if (a.transport != 0) return;
    if (a.status == 426) { out->valid = true, out->needsNewer = true; return; }   // (upgrade_required / update_required)
    if (a.status != 200) return;
    NpNet::Json j;
    if (!j.Parse(a.body.data(), a.body.size()) || j.TypeOf(j.Root()) != NpNet::Json::OBJ || j.TypeOf(j.Get(j.Root(), "proto")) != NpNet::Json::NUM) return;
    out->valid = true;
    const int kill = j.Get(j.Root(), "kill");
    for (int i = 0; i < j.Size(kill) && i < 16; ++i) {
        const std::string& k = j.Str(j.At(kill, i));
        if (k == "all" || k == "rooms") out->killed = true;
    }
    if (j.GetNum(j.Root(), "minProto", 1) > 1) out->needsNewer = true;   // (this build speaks wire protocol 1)
    const int app = j.Get(j.Root(), "app");
    const std::string& min = j.GetStr(app, "min");
    if (!min.empty() && VersionOlder(SMG_APP_VERSION, min.c_str())) out->needsNewer = true;
}

}  // namespace

// ---- the bridge ----------------------------------------------------------------------------------------------------------------
extern "C" {

void mp_service_pump(void) { MP_HOST_ALLOC;
    SvcLock svcLock;
    PumpOne(&g_lookup, OnLookup);
    InvitePump();   // code5
    PumpOne(&g_challenge, OnChallenge);
    PumpOne(&g_register, OnRegister);
    PumpOne(&g_rename, OnRename);
    SolveSlice();
    PumpOne(&g_friends, OnFriends);
    PumpOne(&g_friendOp, OnFriendOp);
    PumpOne(&g_acctDelete, OnAcctDelete);
    PumpOne(&g_board, OnBoard);
    if (g_submit.req.kind) {
        Answer a{2, 0, std::string()};
        if (PollRequest(&g_submit.req, &a) == 1) OnSubmit(a);
    }
    if (g_refreshFriends && g_acct.registered && g_friends.state != -1) {
        g_refreshFriends = false;
        StartFriends();
    }
    // next queued time (oldest first), one at a time
    if (g_inflight < 0 && g_acct.registered) {
        const uint64_t now = NowMs();
        for (int i = 0; i < kSubmitQueue; ++i) {
            Submit& s = g_queue[i];
            if (!s.used || s.notBeforeMs > now) continue;
            if (BusyLeftMs() > 0) { s.notBeforeMs = now + BusyLeftMs(); break; }   // quota32: held while the service said it is busy
            if (StartRequest(&g_submit.req, "POST", "/v1/times", SubmitBody(s), true)) g_inflight = i;
            else s.notBeforeMs = now + 30000;   // quota32: could not start (never dropped for it)
            break;
        }
    }
}

void mp_service_note_code(const char* code) { MP_HOST_ALLOC;
    SvcLock svcLock;
    CopyStr(g_lastCode, sizeof g_lastCode, code ? code : "");
}

bool mp_service_enabled(void) { MP_HOST_ALLOC; return !ServiceUrl().empty(); }

bool mp_service_info_begin(uint32_t timeoutMs) { MP_HOST_ALLOC;
    SvcLock svcLock;
    g_infoReq.Drop();
    if (timeoutMs < 200) timeoutMs = 200;
    if (timeoutMs > 15000) timeoutMs = 15000;
    if (!StartRequest(&g_infoReq, "GET", "/v1/info", std::string(), false, timeoutMs)) return false;
    g_infoStartMs = NowMs();
    return true;
}
int mp_service_info_poll(MpServiceInfo* out) { MP_HOST_ALLOC;
    SvcLock svcLock;
    if (g_infoReq.kind == 0) return -1;
    Answer a{2, 0, std::string()};
    if (PollRequest(&g_infoReq, &a) != 1) return 0;
    if (out) {
        *out = MpServiceInfo();
        ReadInfo(a, out);
        const uint64_t ms = NowMs() - g_infoStartMs;
        out->ms = ms > 600000 ? 600000 : int(ms);
    }
    return 1;
}
void mp_service_info_cancel(void) { MP_HOST_ALLOC; SvcLock svcLock; g_infoReq.Drop(); }

// beta diagnostics: ONE generic authenticated POST for the problem report and the connection check's relay pass (mp_diag.cpp). The same headers,
// token and transports as every other call. One at a time; the caller polls.
namespace { Req g_postReq; }
bool mp_service_post_begin(const char* path, const char* body, size_t len, uint32_t timeoutMs) { MP_HOST_ALLOC;
    SvcLock svcLock;
    if (!path || !Ready("post")) return false;
    if (!g_acct.registered) return false;
    g_postReq.Drop();
    if (timeoutMs < 1000) timeoutMs = 1000;
    if (timeoutMs > 60000) timeoutMs = 60000;
    return StartRequest(&g_postReq, "POST", path, std::string(body ? body : "", body ? len : 0), true, timeoutMs);
}
int mp_service_post_poll(int* transport, int* status, char* out, int cap) { MP_HOST_ALLOC;
    SvcLock svcLock;
    if (g_postReq.kind == 0) return -1;
    Answer a{2, 0, std::string()};
    if (PollRequest(&g_postReq, &a) != 1) return 0;
    if (transport) *transport = a.transport;
    if (status) *status = a.status;
    if (out && cap > 0) {
        const size_t k = a.body.size() < size_t(cap - 1) ? a.body.size() : size_t(cap - 1);
        memcpy(out, a.body.data(), k);
        out[k] = 0;
    }
    return 1;
}
void mp_service_post_cancel(void) { MP_HOST_ALLOC; SvcLock svcLock; g_postReq.Drop(); }
MpUiError mp_service_last_error(void) { MP_HOST_ALLOC; return g_lastError; }
const char* mp_service_last_code(void) { MP_HOST_ALLOC; return g_lastCode; }
int mp_service_register_state(void) { MP_HOST_ALLOC; SvcLock svcLock; mp_service_pump(); return g_register.state; }
int mp_service_friend_op_state(void) { MP_HOST_ALLOC; SvcLock svcLock; mp_service_pump(); return g_friendOp.state; }
int mp_service_account_delete_state(void) { MP_HOST_ALLOC; SvcLock svcLock; mp_service_pump(); return g_acctDelete.state; }
bool mp_service_account_delete(void) { MP_HOST_ALLOC;
    SvcLock svcLock;
    mp_service_pump();
    if (!NeedAccount("account_delete")) return false;
    if (g_acctDelete.state == -1) return false;
    if (!StartRequest(&g_acctDelete.req, "DELETE", "/v1/me", std::string(), true)) {
        FailStart();
        g_acctDelete.state = -2;
        return false;
    }
    g_acctDelete.state = -1;
    mp_log("service account delete asked");
    return true;
}
const char* mp_service_app_version(void) { MP_HOST_ALLOC; return SMG_APP_VERSION; }
const char* mp_service_online_dir(void) { MP_HOST_ALLOC; return OnlineDirC(); }   // MP3 keepsafe
bool mp_service_account_previous(int index, int* pid, char* name, int nameCap) {   // keepfix
    MP_HOST_ALLOC;
    uint8_t b[kStoreSize];
    const std::string path = AccountPath(g_acctUrl.empty() ? ServiceUrl() : g_acctUrl);
    if (!mp_keepsafe_account_previous(path.c_str(), index, b)) return false;
    if (pid) *pid = int(int32_t(Get32(b + 8)));
    if (name && nameCap > 0) {
        char n[kNameBytes];
        memcpy(n, b + 12 + kTokenBytes, kNameBytes);
        n[kNameBytes - 1] = 0;
        snprintf(name, size_t(nameCap), "%s", n);
    }
    memset(b, 0, sizeof b);
    return true;
}

bool mp_service_credentials(char* urlOut, int urlCap, char* tokenOut, int tokenCap) { MP_HOST_ALLOC;
    SvcLock svcLock;
    LoadAccount();
    const std::string u = ServiceUrl();
    if (u.empty() || !g_acct.registered || !urlOut || !tokenOut || urlCap <= int(u.size()) || tokenCap <= int(strlen(g_acct.token))) return false;
    snprintf(urlOut, size_t(urlCap), "%s", u.c_str());
    snprintf(tokenOut, size_t(tokenCap), "%s", g_acct.token);
    return true;
}

void mp_service_shutdown(void) { MP_HOST_ALLOC;
    SvcLock svcLock;
    g_postReq.Drop();
    g_register.req.Drop();
    g_challenge.req.Drop();
    g_rename.req.Drop();
    g_challenge.state = g_rename.state = 0;
    g_es = ES_IDLE;
    g_ensureFails = 0;
    g_ensureRetryAt = 0;
    g_reregs = 0;
    g_hadAccount = false;
    g_nameOverride[0] = g_renameTried[0] = 0;
    g_friends.req.Drop();
    g_friendOp.req.Drop();
    g_acctDelete.req.Drop();
    g_board.req.Drop();
    g_submit.req.Drop();
    g_infoReq.Drop();
    g_register.state = g_friends.state = g_friendOp.state = g_acctDelete.state = g_board.state = 0;
    for (Submit& s : g_queue) s = Submit();
    g_inflight = -1;
    BusyClear();
    g_friendCount = g_boardCount = g_incomingCount = 0;
    g_refreshFriends = false;
    g_lookup.req.Drop();
    g_lookup.state = 0;
    g_invReg.op.req.Drop(); g_invRes.op.req.Drop(); g_invKeep.req.Drop(); g_invClose.req.Drop();   // code5
    g_invReg = Invite(); g_invRes = Invite();
    g_netConfigured = false;
    memset(g_acct.token, 0, sizeof g_acct.token);
    g_acct = Account();
    g_lastError = MP_ERR_NONE;
    g_lastCode[0] = 0;
}

void mp_ui_profile(MpProfile* out) { MP_HOST_ALLOC;
    SvcLock svcLock;
    if (!out) return;
    mp_service_pump();
    LoadAccount();
    *out = MpProfile();
    if (!g_acct.registered) return;
    snprintf(out->name, sizeof out->name, "%s", g_acct.name);
    memcpy(out->friendCode, g_acct.code, sizeof out->friendCode);
    out->registered = true;
}

// service review: silent sign-in (mp_service.h)
int mp_service_ensure_account(void) { MP_HOST_ALLOC;
    SvcLock svcLock;
    mp_service_pump();
    LoadAccount();
    if (ServiceUrl().empty()) { FailLocal(MP_ERR_SERVICE_DOWN, "offline_build"); return -int(MP_ERR_SERVICE_DOWN); }
    if (g_acct.registered) {
        // the profile's name changed: the service follows quietly (once per new name; a refusal keeps the old one)
        char want[MP_NAME_BYTES + 1];
        ServiceName(want);
        if (g_rename.state != -1 && strcmp(want, g_acct.name) != 0 && strcmp(want, g_renameTried) != 0) {
            snprintf(g_renameTried, sizeof g_renameTried, "%s", want);
            std::string body = "{\"name\":";
            NpNet::JsonEscapeAppend(&body, want);
            body += "}";
            if (StartRequest(&g_rename.req, "PUT", "/v1/me", body, true)) g_rename.state = -1;
        }
        return 1;
    }
    if (g_es == ES_CHALLENGE || g_es == ES_SOLVING || g_es == ES_REGISTER) return 0;
    if (g_es == ES_FAILED && NowMs() < g_ensureRetryAt) {
        g_lastError = g_ensureErr;
        CopyStr(g_lastCode, sizeof g_lastCode, g_ensureCode);
        return -int(g_ensureErr);
    }
    if (g_hadAccount) {
        // the service no longer knows this console's account (expired, removed, a restored database): a new one, quietly; but never a
        // loop (a service that refuses every token would otherwise get a new account from this console every few seconds)
        if (g_reregs >= kMaxReregs) {
            RegFailReason("busy");
            g_ensureRetryAt = NowMs() + 3600000u;
            return -int(g_ensureErr);
        }
        ++g_reregs;
        mp_log("service: the account is not known any more: registering again (%d)", g_reregs);
    }
    char name[MP_NAME_BYTES + 1];
    ServiceName(name);
    BeginRegistration(name);
    if (g_es == ES_FAILED) return -int(g_ensureErr);
    return 0;
}

void mp_service_set_name(const char* nameUtf8) { MP_HOST_ALLOC;
    SvcLock svcLock;
    snprintf(g_nameOverride, sizeof g_nameOverride, "%s", nameUtf8 ? nameUtf8 : "");
}

bool mp_ui_register(const char* nameUtf8) { MP_HOST_ALLOC;
    SvcLock svcLock;
    mp_service_pump();
    char clean[MP_NAME_BYTES + 1];
    if (!NameValid(nameUtf8, clean, sizeof clean)) { FailLocal(MP_ERR_BAD_NAME, "bad_name"); mp_log("service register refused: bad name"); return false; }
    if (!Ready("register")) return false;
    if (g_register.state == -1) return false;
    snprintf(g_nameOverride, sizeof g_nameOverride, "%s", clean);
    BeginRegistration(clean);
    return g_register.state == -1;
}

bool mp_ui_friends_request(void) { MP_HOST_ALLOC;
    SvcLock svcLock;
    mp_service_pump();
    if (!NeedAccount("friends")) { g_friends.state = -2; return false; }
    return StartFriends();
}

int mp_ui_friends_get(MpFriend* rows, int max) { MP_HOST_ALLOC;
    SvcLock svcLock;
    mp_service_pump();
    if (g_friends.state == -1) return -1;
    if (g_friends.state != 1) return -2;
    int n = g_friendCount < max ? g_friendCount : max;
    if (n < 0 || !rows) n = 0;
    for (int i = 0; i < n; ++i) rows[i] = g_friendRows[i];
    return n;
}

enum FriendKind { kFriendAdd, kFriendRemove, kFriendDismiss, kFriendBlock, kFriendUnblock };

static bool FriendOp(const char* code, FriendKind kind) { MP_HOST_ALLOC;
    static const char* const kNames[] = {"friend_add", "friend_remove", "friend_dismiss", "friend_block", "friend_unblock"};
    static const char* const kPaths[] = {"/v1/friends", "/v1/friends/remove", "/v1/friends/dismiss", "/v1/friends/block", "/v1/friends/unblock"};
    static const char* const kWords[] = {"add", "remove", "dismiss", "block", "unblock"};
    SvcLock svcLock;
    mp_service_pump();
    char clean[10];
    if (!FriendCodeValid(code, clean)) { FailLocal(MP_ERR_INTERNAL, "bad_code"); mp_log("service friend refused: bad code"); return false; }
    if (!NeedAccount(kNames[kind])) return false;
    if (g_friendOp.state == -1) return false;
    if (kind != kFriendRemove && kind != kFriendUnblock && !strcmp(clean, g_acct.code)) { FailLocal(MP_ERR_INTERNAL, "own_code"); return false; }
    std::string body = std::string("{\"code\":\"") + clean + "\"}";
    if (!StartRequest(&g_friendOp.req, "POST", kPaths[kind], body, true)) {
        FailStart();
        g_friendOp.state = -2;
        return false;
    }
    g_friendOp.state = -1;
    mp_log("service friend %s code=%s", kWords[kind], clean);
    return true;
}

bool mp_ui_friend_add(const char* code) { MP_HOST_ALLOC; return FriendOp(code, kFriendAdd); }
bool mp_ui_friend_remove(const char* code) { MP_HOST_ALLOC; return FriendOp(code, kFriendRemove); }
// MP3: incoming requests (declared in mp_service.h; the contract header mp_bridge.h has no entry for them yet)
bool mp_ui_friend_dismiss(const char* code) { MP_HOST_ALLOC; return FriendOp(code, kFriendDismiss); }
bool mp_ui_friend_block(const char* code) { MP_HOST_ALLOC; return FriendOp(code, kFriendBlock); }
bool mp_ui_friend_unblock(const char* code) { MP_HOST_ALLOC; return FriendOp(code, kFriendUnblock); }

int mp_ui_incoming_get(MpFriend* rows, int max) { MP_HOST_ALLOC;
    SvcLock svcLock;
    mp_service_pump();
    if (g_friends.state == -1) return -1;
    if (g_friends.state != 1) return -2;
    int n = g_incomingCount < max ? g_incomingCount : max;
    if (n < 0 || !rows) n = 0;
    for (int i = 0; i < n; ++i) rows[i] = g_incomingRows[i];
    return n;
}

bool mp_ui_board_request(uint8_t galaxyIndex, uint8_t scenario, uint8_t which) { MP_HOST_ALLOC;
    SvcLock svcLock;
    mp_service_pump();
    if (galaxyIndex > 63 || scenario < 1 || scenario > 8 || which > 2) { FailLocal(MP_ERR_INTERNAL, "bad_param"); return false; }
    if (!NeedAccount("board")) { g_board.state = -2; return false; }
    if (g_board.state == -1) g_board.req.Drop();   // a newer question replaces the running one
    static const char* const kModes[3] = {"top", "near", "friends"};
    char body[96];
    snprintf(body, sizeof body, "{\"galaxy\":%u,\"star\":%u,\"mode\":\"%s\",\"limit\":20}", unsigned(galaxyIndex), unsigned(scenario), kModes[which]);
    if (!StartRequest(&g_board.req, "POST", "/v1/times/get", body, true)) { FailStart(); g_board.state = -2; return false; }
    g_board.state = -1;
    return true;
}

int mp_ui_board_get(MpBoardRow* rows, int max) { MP_HOST_ALLOC;
    SvcLock svcLock;
    mp_service_pump();
    if (g_board.state == -1) return -1;
    if (g_board.state != 1) return -2;
    int n = g_boardCount < max ? g_boardCount : max;
    if (n < 0 || !rows) n = 0;
    for (int i = 0; i < n; ++i) rows[i] = g_boardRows[i];
    return n;
}

void mp_race_submit(uint8_t galaxyIndex, uint8_t scenario, uint32_t frames, const uint8_t* poseDigest16) { MP_HOST_ALLOC;
    // (the session first: MpCore takes its own lock and must never be entered with the service lock held)
    int sessionPlayers = 1;
    MpUiStatus sessionSt;
    memset(&sessionSt, 0, sizeof sessionSt);
    const bool inSession = mp_active();
    if (inSession) {
        sessionPlayers = 0;
        for (int i = 0; i < MP_MAX_PLAYERS; ++i) sessionPlayers += mp_slot_present(i) ? 1 : 0;
        if (sessionPlayers < 1) sessionPlayers = 1;
        mp_ui_status(&sessionSt);
    }
    SvcLock svcLock;
    mp_service_pump();
    if (galaxyIndex > 41 || scenario < 1 || scenario > 8 || frames == 0) return;   // (the service knows 42 galaxies, 0..41)
    if (!NeedAccount("time")) return;
    int slot = -1;
    // quota32: a time of the same star that waits for a busy service (not on the wire, not from a room) is replaced by a faster one, and a
    // slower or equal one is not queued at all (the service keeps only the best of the same kind)
    for (int i = 0; i < kSubmitQueue && !inSession; ++i) {
        const Submit& q = g_queue[i];
        if (!q.used || i == g_inflight || q.room[0] || q.galaxy != galaxyIndex || q.star != scenario || q.players > 1) continue;
        if (frames >= q.frames) { mp_log("service time not queued: a better one for this star is waiting"); return; }
        slot = i;
        break;
    }
    for (int i = 0; i < kSubmitQueue && slot < 0; ++i)
        if (!g_queue[i].used) slot = i;
    if (slot < 0) {   // full: the oldest waiting time (not the one on the wire) gives way to the new one
        for (int i = 0; i < kSubmitQueue; ++i)
            if (i != g_inflight && (slot < 0 || g_queue[i].queuedAt < g_queue[slot].queuedAt)) slot = i;
        if (slot < 0) { mp_log("service time dropped: queue full"); return; }
        mp_log("service time queue full: the oldest waiting time gives way");
    }
    Submit& s = g_queue[slot];
    s = Submit();
    s.queuedAt = NowMs();
    s.used = true;
    s.galaxy = galaxyIndex;
    s.star = scenario;
    s.frames = frames;
    const int players = sessionPlayers;
    if (inSession) {
        const MpUiStatus& st = sessionSt;
        char code[8];
        int n = 0;
        for (int i = 0; i < int(sizeof st.roomCode) && st.roomCode[i]; ++i)
            if (strchr(kCodeAlphabet, st.roomCode[i]) && n < 6) code[n++] = st.roomCode[i];
        if (n == 6) { memcpy(s.room, code, 6); s.room[6] = 0; }
    }
    s.players = uint8_t(players);
    if (players > 1 && !s.room[0]) {
        // the service takes race / co-op times only from a started online room (it answers room_required): a Same Wi-Fi or Nearby session has none
        mp_log("service time not sent: %d players but no online room", players);
        s.used = false;
        return;
    }
    if (poseDigest16) {
        static const char* hex = "0123456789abcdef";
        for (int i = 0; i < 16; ++i) { s.digest[2 * i] = hex[poseDigest16[i] >> 4]; s.digest[2 * i + 1] = hex[poseDigest16[i] & 15]; }
        s.digest[32] = 0;
    }
    mp_log("service time queued galaxy=%u star=%u frames=%u players=%d room=%s", unsigned(galaxyIndex), unsigned(scenario), unsigned(frames), players,
           s.room[0] ? s.room : "-");
    mp_service_pump();
}

// ---- integ: what MpCore and the online transport need from the service (mp_transport.h weak hooks, mp_service.h) ------------------
extern "C" void NpNetConfigureService(const char* baseUrl, const char* token, const char* appVersion, const char* buildHash)
    __attribute__((weak));

void mp_service_set_build(uint32_t fingerprint) { MP_HOST_ALLOC;
    SvcLock svcLock;
    if (fingerprint) snprintf(g_build, sizeof g_build, "%08x", unsigned(fingerprint));
    else g_build[0] = 0;   // (0 = no tag)
    g_netConfigured = false;   // (the next sign-in hands the new value to the online transport)
}

int mp_svc_signin_poll(void) { MP_HOST_ALLOC;
    SvcLock svcLock;
    mp_service_pump();
    if (!Ready("signin")) return -int(MP_ERR_SERVICE_DOWN);
    if (!g_acct.registered) {
        if (g_register.state == -1) return 0;   // the menu's registration is still running
        FailLocal(MP_ERR_SERVICE_DOWN, "not_registered");
        mp_log("service signin: no online account (register first)");
        return -int(MP_ERR_SERVICE_DOWN);
    }
    if (!NpNetConfigureService) {
        FailLocal(MP_ERR_SERVICE_DOWN, "no_transport");
        mp_log("service signin: this build has no online transport");
        return -int(MP_ERR_SERVICE_DOWN);
    }
    if (!g_netConfigured) {
        const std::string url = ServiceUrl();
        NpNetConfigureService(url.c_str(), g_acct.token, SMG_APP_VERSION, g_build);
        g_netConfigured = true;
        mp_log("service signin ok code=%s build=%s", g_acct.code, g_build[0] ? g_build : "-");
    }
    return 1;
}

bool mp_svc_room_lookup_begin(int kind, uint8_t mode, const char* friendCode) { MP_HOST_ALLOC;
    SvcLock svcLock;
    mp_service_pump();
    g_lookupError = MP_ERR_SERVICE_DOWN;
    if (!NeedAccount(kind == 0 ? "quick" : "friend_room")) { g_lookupError = g_lastError; g_lookup.state = -2; return false; }
    if (g_lookup.req.kind) g_lookup.req.Drop();
    std::string path;
    if (kind == 0) {
        path = std::string("/v1/rooms/public?mode=") + (mode == MP_MODE_RACE ? "race" : "coop");
    } else {
        if (!FriendCodeValid(friendCode ? friendCode : "", g_lookupFriend)) {
            FailLocal(MP_ERR_ROOM_NOT_FOUND, "bad_code");
            g_lookupError = MP_ERR_ROOM_NOT_FOUND;
            g_lookup.state = -2;
            return false;
        }
        path = "/v1/friends";
    }
    g_lookupKind = kind == 0 ? 0 : 1;
    if (!StartRequest(&g_lookup.req, "GET", path, std::string(), true)) {
        FailStart();
        g_lookup.state = -2;
        return false;
    }
    g_lookup.state = -1;
    mp_log("service room lookup start kind=%s", g_lookupKind == 0 ? "quick" : "friend");
    return true;
}

int mp_svc_room_lookup_poll(char* codeOut, int cap) { MP_HOST_ALLOC;
    SvcLock svcLock;
    mp_service_pump();
    if (g_lookup.state == -1) return 0;
    if (g_lookup.state == 1) {
        if (codeOut && cap > 0) snprintf(codeOut, size_t(cap), "%s", g_lookupCode);
        g_lookup.state = 0;
        return 1;
    }
    const MpUiError e = g_lookupError != MP_ERR_NONE ? g_lookupError : MP_ERR_SERVICE_DOWN;
    g_lookup.state = 0;
    return -int(e);
}

void mp_svc_room_lookup_cancel(void) { MP_HOST_ALLOC;
    SvcLock svcLock;
    g_lookup.req.Drop();
    g_lookup.state = 0;
}

// The room's mode / visibility / size go to the service when NpNet creates it (NpNetSetCreateBody, mp_conn_online.cpp); the
// service has no "update room" call, so these only log.
void mp_svc_room_hosted(const char* code, const MpSettings* settings, uint8_t visibility, int players) { MP_HOST_ALLOC;
    mp_log("service room hosted code=%s mode=%d visibility=%d players=%d", code ? code : "-", settings ? int(settings->mode) : -1,
           int(visibility), players);
}
void mp_svc_room_closed(const char* code) { MP_HOST_ALLOC; mp_log("service room closed code=%s", code ? code : "-"); }

// ---- code5: far-away invites (port/mp_invite.h) ---------------------------------------------------------------------------------------
bool mp_svc_invite_register_begin(const char* code, const char* blob) { MP_HOST_ALLOC;
    SvcLock svcLock;
    mp_service_pump();
    return InviteBegin(&g_invReg, code, blob, false);
}
int mp_svc_invite_register_poll(int* keepSec) { MP_HOST_ALLOC;
    SvcLock svcLock;
    const int r = InvitePoll(&g_invReg, 0);
    if (r == 1 && keepSec) *keepSec = g_invReg.keepSec;
    return r;
}
void mp_svc_invite_keep(const char* code) { MP_HOST_ALLOC;
    SvcLock svcLock;
    mp_service_pump();
    char c[MP_INVITE_LEN + 1];
    if (!code || mp_invite_normalize(code, c) != MP_INV_TYPED_OK || !NeedAccount("invite_keep")) return;
    g_invKeep.req.Drop();
    if (!StartRequest(&g_invKeep.req, "POST", std::string("/v1/invites/") + c + "/keep", std::string(), true)) mp_log("service invite keep: not started");
}
void mp_svc_invite_close(const char* code) { MP_HOST_ALLOC;
    SvcLock svcLock;
    mp_service_pump();
    char c[MP_INVITE_LEN + 1];
    if (!code || mp_invite_normalize(code, c) != MP_INV_TYPED_OK || !NeedAccount("invite_close")) return;
    // (a register still waiting for its answer belongs to this code: dropped; the service forgets the code by itself after its life)
    if (g_invReg.stage != 0 && !strcmp(g_invReg.code, c)) { g_invReg.op.req.Drop(); g_invReg.stage = 0; }
    g_invClose.req.Drop();
    if (!StartRequest(&g_invClose.req, "POST", std::string("/v1/invites/") + c + "/close", std::string(), true)) mp_log("service invite close: not started");
    else mp_log("service invite closed");
}
bool mp_svc_invite_resolve_begin(const char* code) { MP_HOST_ALLOC;
    SvcLock svcLock;
    mp_service_pump();
    return InviteBegin(&g_invRes, code, 0, true);
}
int mp_svc_invite_resolve_poll(char* blobOut, int cap) { MP_HOST_ALLOC;
    SvcLock svcLock;
    std::string b;
    const int r = InvitePoll(&g_invRes, &b);
    if (r == 1 && blobOut && cap > 0) snprintf(blobOut, size_t(cap), "%s", b.c_str());
    return r;
}
void mp_svc_invite_cancel(void) { MP_HOST_ALLOC;
    SvcLock svcLock;
    g_invReg.op.req.Drop(); g_invRes.op.req.Drop();
    g_invReg.stage = g_invRes.stage = 0;
}

}  // extern "C"

#else  // no np_json.h in this build: online is off (the weak defaults' behaviour, with a reason)

extern "C" {
void mp_service_pump(void) { MP_HOST_ALLOC;}
bool mp_service_enabled(void) { MP_HOST_ALLOC; return false; }
MpUiError mp_service_last_error(void) { MP_HOST_ALLOC; return MP_ERR_SERVICE_DOWN; }
const char* mp_service_last_code(void) { MP_HOST_ALLOC; return "no_net"; }
void mp_service_note_code(const char*) { MP_HOST_ALLOC;}
int mp_service_register_state(void) { MP_HOST_ALLOC; return 0; }
int mp_service_friend_op_state(void) { MP_HOST_ALLOC; return 0; }
bool mp_service_credentials(char*, int, char*, int) { MP_HOST_ALLOC; return false; }
bool mp_service_info_begin(uint32_t) { MP_HOST_ALLOC; return false; }
int mp_service_info_poll(MpServiceInfo*) { MP_HOST_ALLOC; return -1; }
void mp_service_info_cancel(void) { MP_HOST_ALLOC; }
bool mp_service_post_begin(const char*, const char*, size_t, uint32_t) { MP_HOST_ALLOC; return false; }
int mp_service_post_poll(int*, int*, char*, int) { MP_HOST_ALLOC; return -1; }
void mp_service_post_cancel(void) { MP_HOST_ALLOC; }
const char* mp_service_app_version(void) { MP_HOST_ALLOC; return SMG_APP_VERSION; }
const char* mp_service_online_dir(void) { return nullptr; }   // MP3 keepsafe: no account folder in this build (the data-set code uses its own rule)
bool mp_service_account_previous(int, int*, char*, int) { return false; }
void mp_service_shutdown(void) { MP_HOST_ALLOC;}
void mp_ui_profile(MpProfile* out) { MP_HOST_ALLOC; if (out) *out = MpProfile(); }
bool mp_ui_register(const char*) { MP_HOST_ALLOC; return false; }
int mp_service_ensure_account(void) { MP_HOST_ALLOC; return -int(MP_ERR_SERVICE_DOWN); }
void mp_service_set_name(const char*) { MP_HOST_ALLOC; }
bool mp_ui_friends_request(void) { MP_HOST_ALLOC; return false; }
int mp_ui_friends_get(MpFriend*, int) { MP_HOST_ALLOC; return -2; }
bool mp_ui_friend_add(const char*) { MP_HOST_ALLOC; return false; }
bool mp_ui_friend_remove(const char*) { MP_HOST_ALLOC; return false; }
bool mp_ui_friend_dismiss(const char*) { MP_HOST_ALLOC; return false; }
bool mp_ui_friend_block(const char*) { MP_HOST_ALLOC; return false; }
bool mp_ui_friend_unblock(const char*) { MP_HOST_ALLOC; return false; }
int mp_ui_incoming_get(MpFriend*, int) { MP_HOST_ALLOC; return -2; }
bool mp_ui_board_request(uint8_t, uint8_t, uint8_t) { MP_HOST_ALLOC; return false; }
int mp_ui_board_get(MpBoardRow*, int) { MP_HOST_ALLOC; return -2; }
void mp_race_submit(uint8_t, uint8_t, uint32_t, const uint8_t*) { MP_HOST_ALLOC;}
void mp_service_set_build(uint32_t) { MP_HOST_ALLOC;}
int mp_svc_signin_poll(void) { MP_HOST_ALLOC; return -int(MP_ERR_SERVICE_DOWN); }
bool mp_svc_room_lookup_begin(int, uint8_t, const char*) { MP_HOST_ALLOC; return false; }
int mp_svc_room_lookup_poll(char*, int) { MP_HOST_ALLOC; return -int(MP_ERR_SERVICE_DOWN); }
void mp_svc_room_lookup_cancel(void) { MP_HOST_ALLOC;}
void mp_svc_room_hosted(const char*, const MpSettings*, uint8_t, int) { MP_HOST_ALLOC;}
void mp_svc_room_closed(const char*) { MP_HOST_ALLOC;}
bool mp_svc_invite_register_begin(const char*, const char*) { return false; }
int mp_svc_invite_register_poll(int*) { return -int(MP_INV_OFF); }
void mp_svc_invite_keep(const char*) {}
void mp_svc_invite_close(const char*) {}
bool mp_svc_invite_resolve_begin(const char*) { return false; }
int mp_svc_invite_resolve_poll(char*, int) { return -int(MP_INV_OFF); }
void mp_svc_invite_cancel(void) {}
}

#endif
