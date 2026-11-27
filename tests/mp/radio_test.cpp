// Host tests of net/src/vlink_vita_radio.cpp (the Vita ad hoc radio state machine), compiled with -DNP_TARGET_VITA against tests/mp/radio_stub (our own
// stand-in for the Vita ad hoc API: UDP-loopback cell, fault switches). The radio keeps its state in globals, so every station is a forked process;
// the cell (MACs, peer lists, fault switches, counters, log lines) is shared memory. Build and run: tests/mp/radio_build.sh. Real time: about 1 minute.
#include "radio_stub/radio_stub.h"
#include "vlink_vita_radio.h"
#include <psp2/pspnet_adhocctl.h>

#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <time.h>

using namespace radiostub;

struct Ctx { Cell *c; int me, n; Station &s() const { return c->st[me]; } };
static bool g_verbose = false;
static uint64_t ms_now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return uint64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000; }
#define CHECK(ctx, cond, ...) do { if (!(cond)) { Station &s_ = (ctx).s(); if (!s_.fail[0]) snprintf(s_.fail, sizeof s_.fail, "station %d line %d: %s ", (ctx).me, __LINE__, #cond); \
    size_t l_ = strlen(s_.fail); snprintf(s_.fail + l_, sizeof s_.fail - l_, "" __VA_ARGS__); fprintf(stderr, "CHECK FAILED station %d line %d: %s\n", (ctx).me, __LINE__, #cond); } } while (0)

static NpRadioSnapshot snap() { NpRadioSnapshot s{}; np_vita_radio_snapshot(&s); return s; }
static void pump_once() { np_vita_radio_dialog_render(); np_vita_radio_pump(); usleep(1000); }
static bool pump_until(const std::function<bool()> &done, int ms) { const uint64_t end = ms_now() + ms; while (ms_now() < end) { pump_once(); if (done()) return true; } return done(); }
static bool bring_up(Ctx &x, int ms = 8000) {
    np_vita_radio_dialog_prepare(true);
    if (np_vita_radio_begin_link() != NP_RADIO_AGAIN) return false;
    const bool ok = pump_until([] { const auto p = snap().phase; return p == NP_RADIO_OPEN || p == NP_RADIO_DOWN; }, ms);
    if (!ok || snap().phase != NP_RADIO_OPEN) return false;
    x.s().up = 1; return true;
}
static bool close_down(int ms = 8000) { np_vita_radio_close(); return pump_until([] { return snap().phase == NP_RADIO_DOWN; }, ms); }
static bool all(Ctx &x, std::atomic<int> Station::*f, int bit = 0) {
    for (int i = 1; i <= x.n; ++i) { if (bit) { if (!((x.c->st[i].*f).load() & bit)) return false; } else if (!(x.c->st[i].*f).load()) return false; }
    return true;
}
static bool wait_all_up(Ctx &x, int ms = 8000) { return pump_until([&] { return all(x, &Station::up); }, ms); }
static bool wait_bit(Ctx &x, std::atomic<int> Station::*f, int bit, int ms = 20000) { return pump_until([&] { return all(x, f, bit); }, ms); }
static bool wait_flag(Ctx &x, std::atomic<int> Station::*f, int ms = 20000) { return pump_until([&] { return all(x, f); }, ms); }

// Frame exchange: broadcast (the radio adds the peer list's unicasts) every 50 ms; ends when every station has seen every other and said so (phase bit).
struct Frame { char magic[4]; uint8_t from; uint32_t seq; uint8_t pad[23]; };
static int g_macBad = 0;
static uint32_t exchange(Ctx &x, int phase, int ms, bool checkMac = true, bool unicast = true) {
    uint32_t mask = 0, want = 0; for (int i = 1; i <= x.n; ++i) if (i != x.me) want |= 1u << i;
    const uint64_t end = ms_now() + ms; uint64_t nextSend = 0; uint32_t seq = 0;
    while (ms_now() < end) {
        pump_once();
        if (ms_now() >= nextSend) {
            nextSend = ms_now() + 50;
            Frame f{}; memcpy(f.magic, "SMGT", 4); f.from = uint8_t(x.me); f.seq = seq++;
            np_vita_radio_link_send(nullptr, &f, sizeof f);
            if (unicast) for (int i = 1; i <= x.n; ++i) if (i != x.me) { uint8_t m[6]; mac_of(i, m); np_vita_radio_link_send(m, &f, sizeof f); }
        }
        uint8_t mac[6], buf[2048];
        for (int n; (n = np_vita_radio_link_recv(mac, buf, sizeof buf)) > 0;) {
            if (n < int(sizeof(Frame)) || memcmp(buf, "SMGT", 4)) continue;
            const int from = buf[4]; if (from < 1 || from > x.n) continue;
            mask |= 1u << from;
            uint8_t want_mac[6]; mac_of(from, want_mac); if (checkMac && memcmp(mac, want_mac, 6)) ++g_macBad;
        }
        if ((mask & want) == want) x.s().got_all |= phase;
        if (all(x, &Station::got_all, phase)) break;
    }
    return mask & want;
}

typedef std::function<void(Ctx &)> Body;
static int g_pass = 0, g_fail = 0;
static void run(const char *name, int n, const Body &body, const std::function<void(Cell *)> &setup = nullptr, int limit_s = 40) {
    Cell *c = create_cell(n); if (setup) setup(c);
    const uint64_t t0 = ms_now(); std::vector<pid_t> pids;
    for (int i = 1; i <= n; ++i) {
        const pid_t pid = fork();
        if (pid == 0) {
            if (!g_verbose) { freopen("/dev/null", "w", stderr); }
            alarm(limit_s); attach(c, i); Ctx x{c, i, n}; g_macBad = 0;
            body(x);
            _exit(c->st[i].fail[0] ? 1 : 0);
        }
        pids.push_back(pid);
    }
    bool ok = true; std::string why;
    for (int i = 0; i < n; ++i) {
        int st = 0; waitpid(pids[i], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { ok = false; why += "[station " + std::to_string(i + 1) + (WIFSIGNALED(st) ? " killed by signal " + std::to_string(WTERMSIG(st)) : " failed") + ": " + c->st[i + 1].fail + "] "; }
    }
    printf("%s %-34s %5.1fs %s\n", ok ? "PASS" : "FAIL", name, double(ms_now() - t0) / 1000, why.c_str());
    if (!ok) { for (int i = 1; i <= n; ++i) if (!ok && g_verbose) printf("--- log station %d\n%s", i, log_of(c, i).c_str()); }
    ok ? ++g_pass : ++g_fail;
}

// ---- the tests ----
static void link_up(Ctx &x) {
    CHECK(x, bring_up(x), "link did not come up");
    CHECK(x, wait_all_up(x), "not every station up");
    uint8_t own[6]; CHECK(x, np_vita_radio_link_mac(own)); uint8_t want[6]; mac_of(x.me, want); CHECK(x, !memcmp(own, want, 6));
    const uint32_t mask = exchange(x, 1, 10000); uint32_t want_mask = 0; for (int i = 1; i <= x.n; ++i) if (i != x.me) want_mask |= 1u << i;
    CHECK(x, mask == want_mask, "mask %x want %x", mask, want_mask);
    CHECK(x, g_macBad == 0, "%d frames with a wrong source MAC", g_macBad);
    CHECK(x, x.s().rx_bcast.load() > 0 && x.s().rx_ucast.load() > 0, "bcast %d ucast %d", x.s().rx_bcast.load(), x.s().rx_ucast.load());
    // defaults of the switches (= the behaviour before they existed)
    CHECK(x, x.s().dlg_group.load() == 1 && !strcmp(x.s().dlg_group_name, "SMGMP001") && x.s().dlg_timeout_us.load() == 60000000u && x.s().dlg_mode.load() == 5);
    CHECK(x, log_has(x.c, x.me, "Nearby starts: mode conn, group SMGMP001, timeout 60000 ms"));
    char mac[40]; snprintf(mac, sizeof mac, "(02:00:00:00:00:%02X)", x.me); CHECK(x, log_has(x.c, x.me, mac));
    if (x.me == 1) {   // size limits
        uint8_t big[1500] = {}; CHECK(x, np_vita_radio_link_send(nullptr, big, 1445) == -1); CHECK(x, snap().oversize == 1);
        CHECK(x, np_vita_radio_link_send(nullptr, big, 1444) == 1444);
    }
    CHECK(x, pump_until([&] { return log_has(x.c, x.me, "a nearby console is in the cell"); }, 3500), "cell look never logged");
    x.s().done = 1; wait_flag(x, &Station::done);
    CHECK(x, close_down(), "no DOWN"); CHECK(x, snap().phase == NP_RADIO_DOWN);
}
static void faulty_link(Ctx &x, int check) {   // check: 0 bcast broken, 1 swapped port
    CHECK(x, bring_up(x)); CHECK(x, wait_all_up(x));
    const uint32_t mask = exchange(x, 1, 10000, check != 2); uint32_t want_mask = 0; for (int i = 1; i <= x.n; ++i) if (i != x.me) want_mask |= 1u << i;
    CHECK(x, mask == want_mask, "mask %x want %x", mask, want_mask);
    if (check == 0) CHECK(x, x.s().rx_bcast.load() == 0 && x.s().rx_ucast.load() > 0, "bcast %d ucast %d", x.s().rx_bcast.load(), x.s().rx_ucast.load());
    if (check == 1) {
        pump_until([&] { return log_has(x.c, x.me, "PDP port 0x4D4A"); }, 1000);
        CHECK(x, log_has(x.c, x.me, "PDP port 0x4D4A, not 4A4D"), "no byte-order line");
        CHECK(x, snap().sce_error == 0 && snap().rx_dropped == 0 && snap().phase == NP_RADIO_OPEN);
    }
    x.s().done = 1; wait_flag(x, &Station::done); CHECK(x, close_down());
}
static void err_burst(Ctx &x, int burst) {
    CHECK(x, bring_up(x)); CHECK(x, wait_all_up(x)); CHECK(x, exchange(x, 1, 10000) != 0);
    if (burst < 200) {   // R2: a short burst of odd errors does not end the link
        if (x.me == 2) x.s().f.recv_err_burst = burst;
        if (x.me == 2) CHECK(x, pump_until([&] { return x.s().rx_errs_served.load() >= burst; }, 5000), "burst not served");
        x.s().got_all |= 16; wait_bit(x, &Station::got_all, 16);
        const uint32_t m = exchange(x, 2, 10000); CHECK(x, m != 0, "no frames after the burst");
        CHECK(x, snap().phase == NP_RADIO_OPEN, "link ended by %d errors", burst);
        if (x.me == 2) { CHECK(x, log_has(x.c, 2, "PdpRecv error 0x80410001")); }
        x.s().done = 1; wait_flag(x, &Station::done); CHECK(x, close_down());
        if (x.me == 2) CHECK(x, log_has(x.c, 2, "PdpRecv errors in this session: 60"), "no total error count at close");
    } else {             // nothing but errors for about a second still ends it; the reason survives a teardown that also fails (error stays the PdpRecv code)
        if (x.me == 2) { x.s().f.recv_err_burst = burst; x.s().f.disconnect_fail = -1; x.s().f.ctlterm_fail = -1; x.s().f.adhocterm_fail = -1; }
        if (x.me == 2) {
            CHECK(x, pump_until([] { return snap().phase != NP_RADIO_OPEN; }, 6000), "link never ended");
            CHECK(x, snap().sce_error == 0x80410001u, "error 0x%08X", snap().sce_error);
            CHECK(x, pump_until([] { return snap().phase == NP_RADIO_DOWN; }, 8000), "no DOWN");
            CHECK(x, snap().status == NP_RADIO_CLOSED && snap().sce_error == 0x80410001u, "after teardown: status %d error 0x%08X", snap().status, snap().sce_error);
        } else pump_until([&] { return x.c->st[2].done.load() != 0; }, 20000);
        x.s().done = 1; if (x.me == 1) close_down();
    }
}
static void teardown_fail(Ctx &x, bool strict, bool unload) {
    Faults &f = x.s().f; f.disconnect_fail = -1; f.ctlterm_fail = -1; f.adhocterm_fail = -1; f.pdpdelete_fail = -1; f.term_keeps_state = strict ? 1 : 0; f.module_unload_fail = unload ? -1 : 0;
    CHECK(x, bring_up(x), "first session");
    const uint64_t t = ms_now(); CHECK(x, close_down(10000), "R5: failing teardown never reached DOWN, stage %s", snap().stage);
    CHECK(x, ms_now() - t < 6000, "teardown took %d ms", int(ms_now() - t));
    CHECK(x, log_has(x.c, x.me, "three times: going on with the close"));
    CHECK(x, bring_up(x), "R5: second session did not link (stage %s, error 0x%08X)", snap().stage, snap().sce_error);
    CHECK(x, snap().sce_error == 0, "the old teardown code 0x%08X shows as the new session's error", snap().sce_error);
    np_vita_radio_note_leftovers("Same Wi-Fi");   // the last close gave up on a call: noted (and not yet cleared: it was skipped in that close)
    CHECK(x, log_has(x.c, x.me, "Same Wi-Fi opens although the ad hoc close gave up on"), "leftovers not noted");
    f.disconnect_fail = 0; f.ctlterm_fail = 0; f.adhocterm_fail = 0; f.pdpdelete_fail = 0; f.module_unload_fail = 0; f.term_keeps_state = 0;
    CHECK(x, close_down(), "second close"); CHECK(x, bring_up(x), "third session"); CHECK(x, close_down());
    const std::string before = log_of(x.c, x.me); np_vita_radio_note_leftovers("online play");
    CHECK(x, log_of(x.c, x.me) == before, "leftovers still noted after a clean close");
}
static void ctl_adopt_wrong_id(Ctx &x) {   // session 1 falls back to the title id; in session 2 the first Init fails with another error and the fallback meets ALREADY_INITIALIZED
    Faults &f = x.s().f; f.ctl_refuse_first = 1; f.ctlterm_fail = -1; f.term_keeps_state = 1;
    CHECK(x, bring_up(x)); CHECK(x, !strcmp(x.s().dlg_id, "PCSG90000"), "session 1 id %s", x.s().dlg_id); CHECK(x, close_down());
    f.ctl_refuse_smg = 1;
    CHECK(x, bring_up(x), "session 2 (stage %s)", snap().stage); CHECK(x, !strcmp(x.s().dlg_id, "PCSG90000"), "session 2 id %s", x.s().dlg_id);
    CHECK(x, close_down());
    f.ctl_refuse_smg = 0;
    CHECK(x, bring_up(x), "session 3 (stage %s)", snap().stage);
    CHECK(x, !strcmp(x.s().dlg_id, "PCSG90000"), "session 3 adopted the wrong id %s", x.s().dlg_id);
    f.ctlterm_fail = 0; f.term_keeps_state = 0; CHECK(x, close_down());
}
static void ctl_adopt(Ctx &x) {   // the first adhoc id is refused (title-id fallback), then a skipped Term on a strict library, then a second session
    Faults &f = x.s().f; f.ctl_refuse_first = 1; f.ctlterm_fail = -1; f.term_keeps_state = 1;
    CHECK(x, bring_up(x)); CHECK(x, !strcmp(x.s().dlg_id, "PCSG90000"), "first session id %s", x.s().dlg_id);
    CHECK(x, close_down());
    CHECK(x, bring_up(x), "second session (stage %s)", snap().stage);
    CHECK(x, !strcmp(x.s().dlg_id, "PCSG90000"), "the dialog got id %s, not the one the library was initialised with", x.s().dlg_id);
    CHECK(x, log_has(x.c, x.me, "ad hoc id PCSG90000: adopted"), "no 'adopted' line");
    f.ctlterm_fail = 0; f.term_keeps_state = 0; CHECK(x, close_down());
}
static void mac_warn(Ctx &x, int mode) {   // 0 zero, 1 first byte; station 1 has the fault
    CHECK(x, bring_up(x)); CHECK(x, wait_all_up(x)); exchange(x, 1, 10000, false);
    if (mode == 0) { if (x.me == 1) CHECK(x, log_has(x.c, 1, "WARNING: our own ad hoc MAC is all zero")); else CHECK(x, pump_until([&] { return log_has(x.c, 2, "00:00:00:00:00:00: ALL ZERO"); }, 1500)); }
    else if (x.me == 2) CHECK(x, pump_until([&] { return log_has(x.c, 2, "first datagram from 42:00:00:00:00:01: FIRST BYTE differs"); }, 1500));
    if (x.me == 2 && mode == 0) CHECK(x, !log_has(x.c, 2, "WARNING: our own"));
    x.s().done = 1; wait_flag(x, &Station::done); CHECK(x, close_down());
}
static void dialog_end(Ctx &x, int res) {   // 1 cancelled, 2 failed; then a good session
    x.s().f.dialog_result = res; np_vita_radio_dialog_prepare(true);
    CHECK(x, np_vita_radio_begin_link() == NP_RADIO_AGAIN);
    CHECK(x, pump_until([] { return snap().phase == NP_RADIO_DOWN; }, 8000), "no DOWN, stage %s", snap().stage);
    CHECK(x, snap().status == NP_RADIO_CLOSED && snap().sce_error == (res == 1 ? 1u : 0x80100C06u), "status %d error 0x%08X", snap().status, snap().sce_error);
    CHECK(x, log_has(x.c, 1, res == 1 ? "cancelled by the player" : "not connected"));
    x.s().f.dialog_result = 0; CHECK(x, bring_up(x), "session after the dialog ended badly"); CHECK(x, close_down());
}
static void vanish(Ctx &x, bool watch) {
    CHECK(x, bring_up(x)); CHECK(x, wait_all_up(x)); CHECK(x, exchange(x, 1, 10000) != 0);
    x.s().got_all |= 4; wait_bit(x, &Station::got_all, 4);   // (all stations leave the exchange before the suspend)
    if (x.me == 3) {
        x.s().f.vanish = 1; const uint64_t t = ms_now();
        CHECK(x, pump_until([] { return snap().phase != NP_RADIO_OPEN; }, 6000), "the cell watch never saw the station vanish");
        const uint64_t took = ms_now() - t;
        if (watch) { CHECK(x, took > 900 && took < 4500, "detected after %d ms", int(took)); CHECK(x, log_has(x.c, 3, "ad hoc cell gone")); }
        else CHECK(x, took < 600, "LINKWATCH=0 took %d ms", int(took));
        CHECK(x, pump_until([] { return snap().phase == NP_RADIO_DOWN; }, 8000)); CHECK(x, snap().status == NP_RADIO_CLOSED);
        x.s().done = 1;
    } else {
        int got = 99; const bool gone = pump_until([&] { int sz = 0, rc = sceNetAdhocctlGetPeerList(&sz, nullptr); got = sz / int(sizeof(SceNetAdhocctlPeerInfo)); return rc >= 0 && got == 1; }, 3000);
        CHECK(x, gone, "peer list still shows %d", got);
        CHECK(x, snap().phase == NP_RADIO_OPEN, "the others' links stay open");
        pump_until([&] { return x.c->st[3].done.load() != 0; }, 15000);
        x.s().done = 1; CHECK(x, close_down());
    }
}
static void injected_strangers(Ctx &x) {   // ADVISED 6: strangers' empty datagrams must not fill the sender table
    CHECK(x, bring_up(x)); CHECK(x, wait_all_up(x));
    if (x.me == 1) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0); sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(uint16_t(x.c->base_port + 2));
        for (int i = 0; i < 12; ++i) { uint8_t d[12] = {1, 0, 0x4D, 0x4A, 0x4D, 0x4A, 0x0A, 0x00, 0x00, 0x00, uint8_t(0x50 + i), 0}; sendto(fd, d, sizeof d, 0, (sockaddr *)&a, sizeof a); }
        close(fd);
    }
    x.s().got_all |= 8; wait_bit(x, &Station::got_all, 8);
    exchange(x, 2, 10000, false);
    if (x.me == 2) { CHECK(x, pump_until([&] { return log_has(x.c, 2, "first datagram from 02:00:00:00:00:01"); }, 2000), "a stranger's empty datagrams filled the sender table"); }
    x.s().done = 1; wait_flag(x, &Station::done); CHECK(x, close_down());
}
static void oversize_peers(Ctx &x) {   // REQUIRED 1: a firmware that reports a longer peer list than the buffer holds
    CHECK(x, bring_up(x)); CHECK(x, wait_all_up(x));
    CHECK(x, exchange(x, 1, 10000) != 0, "broadcast still links");
    if (x.me == 2) CHECK(x, pump_until([&] { return log_has(x.c, 2, "first datagram from 02:00:00:00:00:01: peer list unreadable"); }, 1500), "verdict");
    x.s().done = 1; wait_flag(x, &Station::done); CHECK(x, close_down());
}
static void switches(Ctx &x, int which) {
    Faults &f = x.s().f;
    switch (which) {
    case 0: setenv("SMG_LAN_GROUP", "0", 1); break;
    case 1: f.dialog_refuse_group = 1; break;
    case 2: setenv("SMG_LAN_TIMEOUT_MS", "100", 1); f.dialog_ms = 400; break;
    case 3: setenv("SMG_LAN_TIMEOUT_MS", "0", 1); f.dialog_ms = 300; break;
    case 4: setenv("SMG_LAN_TIMEOUT_MS", "abc", 1); break;
    case 5: setenv("SMG_LAN_TIMEOUT_MS", "99999999", 1); break;
    case 6: setenv("SMG_LAN_DIALOG", "create", 1); break;
    case 7: setenv("SMG_LAN_DIALOG", "join", 1); break;
    case 8: setenv("SMG_LAN_TIMEOUT_MS", "0", 1); f.dialog_ms = 5000; break;
    case 9: setenv("SMG_LAN_TIMEOUT_MS", "-5", 1); break;   // abort path with no time-out
    }
    if (which == 8) {
        np_vita_radio_dialog_prepare(true); CHECK(x, np_vita_radio_begin_link() == NP_RADIO_AGAIN);
        pump_until([] { return snap().phase == NP_RADIO_DIALOG; }, 3000); pump_until([] { return false; }, 300);
        np_vita_radio_close();   // the player cancels / the console suspends while the dialog is up
        CHECK(x, pump_until([] { return snap().phase == NP_RADIO_DOWN; }, 8000), "close during the dialog (timeout 0) never reached DOWN: %s", snap().stage);
        CHECK(x, x.s().dlg_timeout_us.load() == 0);
        f.dialog_ms = 100; CHECK(x, bring_up(x), "session after the abort"); CHECK(x, close_down()); return;
    }
    if (which == 2) {
        np_vita_radio_dialog_prepare(true); CHECK(x, np_vita_radio_begin_link() == NP_RADIO_AGAIN);
        CHECK(x, pump_until([] { return snap().phase == NP_RADIO_DOWN; }, 8000), "dialog time-out never ended the session");
        CHECK(x, snap().status == NP_RADIO_CLOSED && x.s().dlg_timeout_us.load() == 100000u, "timeout us %u", x.s().dlg_timeout_us.load());
        CHECK(x, log_has(x.c, 1, "timeout 100 ms")); return;
    }
    CHECK(x, bring_up(x), "link"); 
    const char *line = nullptr; uint32_t tmo = 60000000u; int group = 1, mode = 5;
    switch (which) {
    case 0: group = 0; line = "Nearby starts: mode conn, group none, timeout 60000 ms"; CHECK(x, !log_has(x.c, 1, "a fallback"), "SMG_LAN_GROUP=0 called a fallback"); break;
    case 1: group = 0; CHECK(x, log_has(x.c, 1, "a fallback")); line = "Nearby starts: mode conn, group SMGMP001"; break;
    case 3: tmo = 0; line = "timeout 0 ms"; break;
    case 4: case 9: line = "timeout 60000 ms"; break;
    case 5: tmo = 3600000u * 1000u; line = "timeout 3600000 ms"; break;
    case 6: mode = 6; line = "mode create"; break;
    case 7: mode = 7; line = "mode join"; break;
    }
    CHECK(x, x.s().dlg_group.load() == group, "group %d", x.s().dlg_group.load()); CHECK(x, x.s().dlg_timeout_us.load() == tmo, "timeout %u", x.s().dlg_timeout_us.load());
    CHECK(x, x.s().dlg_mode.load() == mode, "mode %d", x.s().dlg_mode.load()); if (line) CHECK(x, log_has(x.c, 1, line), "log line %s", line);
    CHECK(x, close_down());
}

int main(int argc, char **argv) {
    g_verbose = getenv("RADIO_TEST_VERBOSE") != nullptr; const char *only = argc > 1 ? argv[1] : nullptr;
    signal(SIGPIPE, SIG_IGN); setvbuf(stdout, nullptr, _IONBF, 0);
    auto want = [&](const char *n) { return !only || strstr(n, only); };
#define T(name, n, ...) if (want(name)) run(name, n, __VA_ARGS__)
    T("link2", 2, link_up);
    T("link4", 4, link_up);
    T("bcast_broken_links_by_unicast", 3, [](Ctx &x) { faulty_link(x, 0); }, [](Cell *c) { for (int i = 1; i <= 3; ++i) c->st[i].f.broadcast_broken = 1; });
    T("swapped_sport_R1", 2, [](Ctx &x) { faulty_link(x, 1); }, [](Cell *c) { for (int i = 1; i <= 2; ++i) c->st[i].f.swap_sport = 1; });
    T("error_burst_recovers_R2", 2, [](Ctx &x) { err_burst(x, 60); });
    T("error_flood_ends_link_keeps_reason", 2, [](Ctx &x) { err_burst(x, 400); });
    T("teardown_fails_second_session_R5", 1, [](Ctx &x) { teardown_fail(x, false, false); });
    T("teardown_strict_library_R5", 1, [](Ctx &x) { teardown_fail(x, true, false); });
    T("module_unload_fails_R5", 1, [](Ctx &x) { teardown_fail(x, false, true); });
    T("zero_mac_warns", 2, [](Ctx &x) { mac_warn(x, 0); }, [](Cell *c) { c->st[1].f.zero_mac = 1; });
    T("first_byte_mac_warns", 2, [](Ctx &x) { mac_warn(x, 1); }, [](Cell *c) { c->st[1].f.firstbyte_mac = 1; });
    T("dialog_cancelled", 1, [](Ctx &x) { dialog_end(x, 1); });
    T("dialog_failed", 1, [](Ctx &x) { dialog_end(x, 2); });
    T("vanished_station_cell_watch", 3, [](Ctx &x) { vanish(x, true); });
    T("vanished_station_linkwatch0", 3, [](Ctx &x) { setenv("SMG_LAN_LINKWATCH", "0", 1); vanish(x, false); });
    T("peer_list_oversize_R1_review", 2, oversize_peers, [](Cell *c) { for (int i = 1; i <= 2; ++i) c->st[i].f.peerlist_oversize = 8 * int(sizeof(SceNetAdhocctlPeerInfo)); });
    T("stranger_datagrams_sender_table", 2, injected_strangers);
    T("switch_group0", 1, [](Ctx &x) { switches(x, 0); });
    T("switch_group_refused_fallback", 1, [](Ctx &x) { switches(x, 1); });
    T("switch_timeout_100_ends_dialog", 1, [](Ctx &x) { switches(x, 2); });
    T("switch_timeout_0", 1, [](Ctx &x) { switches(x, 3); });
    T("switch_timeout_garbage", 1, [](Ctx &x) { switches(x, 4); });
    T("switch_timeout_negative", 1, [](Ctx &x) { switches(x, 9); });
    T("ctl_adopt_keeps_fallback_id", 1, ctl_adopt);
    T("ctl_adopt_after_fallback_already", 1, ctl_adopt_wrong_id);
    T("switch_timeout_clamped", 1, [](Ctx &x) { switches(x, 5); });
    T("switch_dialog_create", 1, [](Ctx &x) { switches(x, 6); });
    T("switch_dialog_join", 1, [](Ctx &x) { switches(x, 7); });
    T("switch_timeout_0_abort", 1, [](Ctx &x) { switches(x, 8); });
    printf("radio_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
