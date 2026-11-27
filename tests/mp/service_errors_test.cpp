// Host test of platform/src/mp/mp_service.cpp against tests/mp/hostile_server.mjs: every kind of broken answer (HTTP errors, malformed or
// hostile JSON, truncation, resets, silence) must end in a failed state and the right MpUiError, never a hang, crash or bad value.
// Run through tests/mp/service_test.sh (SMG_HOSTILE_URL = the hostile server, SMG_ONLINE_TIMEOUT_MS short).
#include <chrono>
#include "mp_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <string>

static int g_fail = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } \
    } while (0)

extern "C" void NpNetConfigureService(const char*, const char*, const char*, const char*) {}

static std::string g_root, g_base;
static int g_dirs = 0;

static void Mode(const char* m) { setenv("SMG_ONLINE_URL", (g_base + "/" + m).c_str(), 1); }

// A fresh registered account (against the hostile server's "ok" mode), then the given mode for what follows.
static void Account(const char* mode) {
    mp_service_shutdown();
    const std::string d = g_root + "/d" + std::to_string(g_dirs++);
    setenv("SMG_ONLINE_DIR", d.c_str(), 1);
    Mode("ok");
    CHECK(mp_ui_register("Tester"));
    for (int i = 0; i < 500 && mp_service_register_state() == -1; ++i) usleep(10000);
    CHECK(mp_service_register_state() == 1);
    Mode(mode);
}

static double Now() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9; }

// Waits while fn() == -1 (pending). Fails the test (returns -1000) when it is still pending after 6 s: that would be a hang.
template <class F> static int Wait(F fn) {
    const double t0 = Now();
    for (;;) {
        mp_service_pump();
        const int r = fn();
        if (r != -1) return r;
        if (Now() - t0 > 6.0) { fprintf(stderr, "HANG\n"); ++g_fail; return -1000; }
        usleep(5000);
    }
}

// quota32: how many requests the hostile server has seen for "<mode><path>" (a plain socket: the client under test is not used for this).
static int HitsOf(const std::string& key) {
    const std::string base = g_base.substr(7);   // 127.0.0.1:PORT
    const size_t colon = base.find(':');
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(atoi(base.c_str() + colon + 1)));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { close(fd); return -1; }
    const std::string rq = "GET /hits/x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    if (write(fd, rq.data(), rq.size()) < 0) { close(fd); return -1; }
    std::string out;
    char buf[2048];
    for (ssize_t n; (n = read(fd, buf, sizeof buf)) > 0;) out.append(buf, size_t(n));
    close(fd);
    const std::string k2 = "\"" + key + "\":";   // keys are "<mode><path>", e.g. "cf1027/v1/friends"
    const size_t p = out.find(k2);
    return p == std::string::npos ? 0 : atoi(out.c_str() + p + k2.size());
}

struct Case {
    const char* mode;
    MpUiError err;   // the error every failing operation must report
    bool lastCode;   // the service's own code is kept
};

int main() {
    const char* h = getenv("SMG_HOSTILE_URL");
    if (!h || !*h) { fprintf(stderr, "set SMG_HOSTILE_URL (service_test.sh does)\n"); return 2; }
    g_base = h;
    char tmpl[] = "/tmp/mp_svc_err.XXXXXX";
    g_root = mkdtemp(tmpl);

    static const Case kCases[] = {
        {"s500", MP_ERR_SERVICE_DOWN, true},  {"s502", MP_ERR_SERVICE_DOWN, false}, {"s503", MP_ERR_SERVICE_DOWN, true},
        {"s429", MP_ERR_RATE_LIMITED, true},  {"s401", MP_ERR_SERVICE_DOWN, true},  {"s426", MP_ERR_UPDATE_REQUIRED, true},
        {"s403", MP_ERR_INTERNAL, true},      {"s404", MP_ERR_INTERNAL, true},      {"s400mismatch", MP_ERR_DIFFERENT_VERSION, true},
        {"bad200", MP_ERR_SERVICE_DOWN, false}, {"html200", MP_ERR_SERVICE_DOWN, false}, {"empty200", MP_ERR_SERVICE_DOWN, false},
        {"redirect", MP_ERR_SERVICE_DOWN, false}, {"trunc", MP_ERR_SERVICE_DOWN, false}, {"rst", MP_ERR_SERVICE_DOWN, false},
        {"huge", MP_ERR_SERVICE_DOWN, false}, {"hang", MP_ERR_SERVICE_DOWN, false}, {"deep", MP_ERR_SERVICE_DOWN, false},
        {"wrongtypes", MP_ERR_NONE, false},
        {"cf1027", MP_ERR_SERVICE_DOWN, true}, {"cf1027html", MP_ERR_SERVICE_DOWN, true}, {"cf1101", MP_ERR_SERVICE_DOWN, true},
        {"overbudget", MP_ERR_SERVICE_DOWN, true},   // quota32: Cloudflare's own pages / the Worker's busy answer: "online play is busy"
    };
    for (const Case& c : kCases) {
        fprintf(stderr, "-- %s\n", c.mode);
        const bool answers = c.err == MP_ERR_NONE;   // "wrongtypes": a 200 whose content is junk: the lists come back empty, not failed
        MpFriend fr[8];
        MpBoardRow rows[8];
        char code[16];
        // friends list
        Account(c.mode);
        CHECK(mp_ui_friends_request());
        int r = Wait([&] { return mp_ui_friends_get(fr, 8); });
        if (answers) CHECK(r == 0); else { CHECK(r == -2); CHECK(mp_service_last_error() == c.err); }
        if (c.lastCode && !answers) CHECK(mp_service_last_code()[0] != 0);
        // board
        Account(c.mode);
        CHECK(mp_ui_board_request(5, 2, 0));
        r = Wait([&] { return mp_ui_board_get(rows, 8); });
        if (answers) CHECK(r == 0); else { CHECK(r == -2); CHECK(mp_service_last_error() == c.err); }
        // friend add
        Account(c.mode);
        CHECK(mp_ui_friend_add("K7QM-4DXT"));
        r = Wait([&] { return mp_service_friend_op_state(); });
        CHECK(r == -2 || answers);
        if (!answers) CHECK(mp_service_last_error() == c.err);
        // quick-match lookup
        Account(c.mode);
        CHECK(mp_svc_room_lookup_begin(0, MP_MODE_RACE, ""));
        r = Wait([&] { const int p = mp_svc_room_lookup_poll(code, sizeof code); return p == 0 ? -1 : p; });
        if (answers) CHECK(r == 1 && code[0] == 0); else CHECK(r == -int(c.err));
        // a friend's room lookup
        Account(c.mode);
        CHECK(mp_svc_room_lookup_begin(1, 0, "K7QM-4DXT"));
        r = Wait([&] { const int p = mp_svc_room_lookup_poll(code, sizeof code); return p == 0 ? -1 : p; });
        CHECK(r < 0);
        // register (no account yet)
        mp_service_shutdown();
        setenv("SMG_ONLINE_DIR", (g_root + "/d" + std::to_string(g_dirs++)).c_str(), 1);
        Mode(c.mode);
        CHECK(mp_ui_register("Tester"));
        r = Wait([&] { return mp_service_register_state(); });
        // (service review: a failed sign-up is one of four reasons for the menu: busy / no_wifi / clock / update)
        {
            const MpUiError want = (c.err == MP_ERR_NO_WIFI || c.err == MP_ERR_UPDATE_REQUIRED) && !answers ? c.err : MP_ERR_SERVICE_DOWN;
            CHECK(r == -2);
            CHECK(mp_service_last_error() == want);
            const char* lc = mp_service_last_code();
            CHECK(!strcmp(lc, "busy") || !strcmp(lc, "no_wifi") || !strcmp(lc, "update"));
        }
        MpProfile pr;
        mp_ui_profile(&pr);
        CHECK(!pr.registered);   // a bad answer never makes an account
        // a time submit: queued, retried or dropped, never stuck or crashing (pump for a while)
        Account(c.mode);
        mp_race_submit(5, 2, 3600, nullptr);
        for (int i = 0; i < 60; ++i) { mp_service_pump(); usleep(5000); }
    }
    mp_service_shutdown();

    // 401 means the service forgot this account: the client offers to register again
    {
        fprintf(stderr, "-- s401 account\n");
        Account("s401");
        MpProfile pr;
        mp_ui_profile(&pr);
        CHECK(pr.registered);
        CHECK(mp_ui_friends_request());
        Wait([&] { MpFriend f[2]; return mp_ui_friends_get(f, 2); });
        mp_ui_profile(&pr);
        CHECK(!pr.registered);
        CHECK(!mp_ui_friends_request());
    }

    // service-side codes map to the right errors
    {
        fprintf(stderr, "-- s409full / s400name\n");
        Account("s409full");
        CHECK(mp_ui_friends_request());
        Wait([&] { MpFriend f[2]; return mp_ui_friends_get(f, 2); });
        CHECK(mp_service_last_error() == MP_ERR_ROOM_FULL);
        mp_service_shutdown();
        setenv("SMG_ONLINE_DIR", (g_root + "/d" + std::to_string(g_dirs++)).c_str(), 1);
        Mode("s400name");
        CHECK(mp_ui_register("Tester"));
        // a name the service refuses is retried as "Player" (and with a fresh challenge) a few times, then "busy": never a name screen
        CHECK(Wait([&] { return mp_service_register_state(); }) == -2);
        CHECK(mp_service_last_error() == MP_ERR_SERVICE_DOWN && !strcmp(mp_service_last_code(), "busy"));
    }

    // a service that asks for 20 bits of proof of work (minutes on a console): the client does not grind, it says "busy" at once
    {
        fprintf(stderr, "-- powhard\n");
        mp_service_shutdown();
        setenv("SMG_ONLINE_DIR", (g_root + "/d" + std::to_string(g_dirs++)).c_str(), 1);
        Mode("powhard");
        CHECK(mp_ui_register("Tester"));
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(Wait([&] { return mp_service_register_state(); }) == -2);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(secs < 5.0);   // (grinding 2^20 tries at 6 ms of every 16 ms would take far longer)
        CHECK(mp_service_last_error() == MP_ERR_SERVICE_DOWN && !strcmp(mp_service_last_code(), "busy"));
        MpProfile pr;
        mp_ui_profile(&pr);
        CHECK(!pr.registered);
    }

    // hostile list contents: bad rows are skipped, good ones kept, nothing overflows
    {
        fprintf(stderr, "-- hostilerows / manynodes\n");
        Account("hostilerows");
        MpFriend fr[64];
        CHECK(mp_ui_friends_request());
        const int n = Wait([&] { return mp_ui_friends_get(fr, 64); });
        CHECK(n == 2);   // "ok" and "Fine Name"
        for (int i = 0; i < n; ++i) CHECK(strlen(fr[i].name) <= MP_NAME_BYTES);
        MpBoardRow rows[64];
        CHECK(mp_ui_board_request(5, 2, 0));
        const int m = Wait([&] { return mp_ui_board_get(rows, 64); });
        CHECK(m == 2);
        mp_service_shutdown();
        Account("manynodes");
        CHECK(mp_ui_friends_request());
        const int k = Wait([&] { return mp_ui_friends_get(fr, 64); });
        CHECK(k <= 50);   // too many nodes for the parser: refused or capped, never more than the table holds
    }

    // a bad registration answer (short token, negative pid, markup in the name) never creates an account
    for (const char* m : {"badregister", "badregister2", "badregister3"}) {
        fprintf(stderr, "-- %s\n", m);
        mp_service_shutdown();
        setenv("SMG_ONLINE_DIR", (g_root + "/d" + std::to_string(g_dirs++)).c_str(), 1);
        Mode(m);
        CHECK(mp_ui_register("Tester"));
        CHECK(Wait([&] { return mp_service_register_state(); }) == -2);
        MpProfile pr;
        mp_ui_profile(&pr);
        CHECK(!pr.registered);
        struct stat st;
        CHECK(stat((g_root + "/d" + std::to_string(g_dirs - 1) + "/online.dat").c_str(), &st) != 0);
    }

    // nothing reachable at all: the port is closed
    {
        fprintf(stderr, "-- closed port\n");
        Account("ok");
        setenv("SMG_ONLINE_URL", "http://127.0.0.1:1", 1);
        CHECK(mp_ui_friends_request());
        CHECK(Wait([&] { MpFriend f[2]; return mp_ui_friends_get(f, 2); }) == -2);
        CHECK(mp_service_last_error() == MP_ERR_SERVICE_DOWN);
    }

    // service review: a console clock years off. The TLS layer says CLOCK (or refuses a certificate's dates): the menus get MP_ERR_CLOCK
    // and "clock" ("set the date and time"); an untrusted certificate is a certificate problem, not the clock
    {
        fprintf(stderr, "-- clock\n");
        const struct { const char* url; MpUiError want; const char* code; } cases[] = {
            {"https://clock.invalid", MP_ERR_CLOCK, "clock"},
            {"https://certdate.invalid", MP_ERR_CLOCK, "clock"},
            {"https://cert.invalid", MP_ERR_SERVICE_DOWN, "busy"},
        };
        for (const auto& c : cases) {
            mp_service_shutdown();
            setenv("SMG_ONLINE_DIR", (g_root + "/d" + std::to_string(g_dirs++)).c_str(), 1);
            setenv("SMG_ONLINE_URL", c.url, 1);
            const int r = Wait([] { const int e = mp_service_ensure_account(); return e == 0 ? -1 : e; });
            CHECK(r == -int(c.want));
            CHECK(!strcmp(mp_service_last_code(), c.code));
        }
    }

    // quota32: the free plan's day is spent. Every path says the same plain thing ("service_busy"), the second call is not even sent (the busy
    // gate), and a queued race time is kept and delivered once the service answers again.
    {
        fprintf(stderr, "-- quota\n");
        setenv("SMG_BUSY_MS", "400", 1);
        for (const char* mode : {"cf1027", "cf1027html", "cf1101", "overbudget"}) {
            Account(mode);
            MpFriend fr[4];
            const int f0 = HitsOf(std::string(mode) + "/v1/friends"), b0 = HitsOf(std::string(mode) + "/v1/times/get");   // (the table above used these modes too)
            CHECK(mp_ui_friends_request());
            CHECK(Wait([&] { return mp_ui_friends_get(fr, 4); }) == -2);
            CHECK(mp_service_last_error() == MP_ERR_SERVICE_DOWN);
            CHECK(!strcmp(mp_service_last_code(), "service_busy"));
            CHECK(HitsOf(std::string(mode) + "/v1/friends") == f0 + 1);
            CHECK(!mp_ui_friends_request());   // held back: nothing is sent while the gate is closed
            CHECK(!strcmp(mp_service_last_code(), "service_busy"));
            CHECK(HitsOf(std::string(mode) + "/v1/friends") == f0 + 1);
            CHECK(!mp_ui_board_request(5, 2, 0));
            CHECK(!strcmp(mp_service_last_code(), "service_busy"));
            CHECK(HitsOf(std::string(mode) + "/v1/times/get") == b0);
        }
        // a race time queued while the service is busy is neither dropped nor hammered, and arrives when the service is back
        Account("cf1027");
        const int before = HitsOf("ok/v1/times"), cf0 = HitsOf("cf1027/v1/times");
        mp_race_submit(3, 2, 4321, nullptr);
        for (int i = 0; i < 100; ++i) { mp_service_pump(); usleep(10000); }
        CHECK(HitsOf("cf1027/v1/times") == cf0 + 1);
        Mode("ok");
        const double t0 = Now();
        while (Now() - t0 < 14.0 && HitsOf("ok/v1/times") == before) { mp_service_pump(); usleep(10000); }
        CHECK(HitsOf("ok/v1/times") == before + 1);
        unsetenv("SMG_BUSY_MS");
    }

    mp_service_shutdown();
    if (g_fail) { fprintf(stderr, "service_errors_test: %d check(s) failed\n", g_fail); return 1; }
    fprintf(stderr, "service_errors_test: all checks passed\n");
    return 0;
}
