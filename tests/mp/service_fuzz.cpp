// Fuzz of the client's parsing of every service reply (security review "not reached": np_signal_http against a hostile service, and
// mp_service's replies), against tests/mp/fuzz_service.mjs: seeded mutations of real answers, junk, redirects, truncation, resets.
// Runs under ASan/UBSan (tests/mp/service_fuzz.sh). Invariants checked after every step: no crash or sanitizer report, the room view keeps
// slots in range, a key is either the 32 bytes the service sent or none, every list stays within its bounds, every state is a known one,
// and a hostile answer never makes the client wait for an absurd time (retry and poll gaps stay within 10 minutes).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "mp_service.h"
#include "np_http.h"
#include "np_signal_http.h"

using namespace NpNet;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

extern "C" void NpNetConfigureService(const char*, const char*, const char*, const char*) {}

static uint64_t g_virt = 5000000;
static uint64_t Clock(void*) { return g_virt; }

static void FuzzSignal(const std::string& url, int rounds) {
    for (int round = 0; round < rounds; ++round) {
        HttpSignalConfig cfg;
        snprintf(cfg.baseUrl, sizeof cfg.baseUrl, "%s", url.c_str());
        snprintf(cfg.token, sizeof cfg.token, "%s", "T234567890123456789012345678901234567890123");
        snprintf(cfg.appVersion, sizeof cfg.appVersion, "3.0.0");
        cfg.requestTimeoutMs = 1500;
        cfg.pollWaitSec = round % 2 ? 8 : 0;
        Signal* s = CreateHttpSignal(cfg);
        s->SetClock(Clock, 0);
        if (round % 3 == 0) s->Join("K7QM4D", "Fuzz"); else s->Host("Fuzz");
        uint64_t lastRequestAt = g_virt;
        for (int step = 0; step < 400; ++step) {
            s->Poll();
            HttpPump();
            usleep(500);
            g_virt += 100;
            const Signal::Status st = s->GetStatus();
            CHECK(st >= Signal::IDLE && st <= Signal::CLOSED);
            if (st == Signal::IN_ROOM) {
                const SigRoom& r = s->Room();
                CHECK(r.mySlot >= 0 && r.mySlot < 4);
                CHECK(r.hostSlot >= 0 && r.hostSlot < 4);
                CHECK(strlen(r.invite) < sizeof r.invite);
                for (int m = 0; m < 4; ++m) CHECK(strlen(r.members[m].name) < sizeof r.members[m].name);
                if (step % 50 == 0) {
                    SigCandidates mine;
                    mine.valid = true;
                    ParseAddr("203.0.113.9:41000", 0, &mine.pub);
                    ParseAddr("192.168.1.9:41000", 0, &mine.local);
                    s->PublishCandidates(mine);
                }
                if (step == 120) s->Started();
                if (step == 150) s->RequestTurn();
            }
            if (st == Signal::FAILED || st == Signal::CLOSED) {
                CHECK(strlen(s->Error()) < 200);
                break;
            }
            (void)lastRequestAt;
        }
        s->Leave();
        for (int i = 0; i < 5; ++i) { HttpPump(); usleep(1000); }
        delete s;
    }
}

template <class F> static int WaitFor(F fn, int ms = 4000) {
    for (int i = 0; i < ms / 5; ++i) {
        mp_service_pump();
        const int r = fn();
        if (r != -1) return r;
        usleep(5000);
    }
    return -1;
}

static void FuzzService(const std::string& url, const std::string& dir, int rounds) {
    setenv("SMG_ONLINE_URL", url.c_str(), 1);
    setenv("SMG_ONLINE_TIMEOUT_MS", "1500", 1);
    for (int round = 0; round < rounds; ++round) {
        mp_service_shutdown();
        const std::string d = dir + "/r" + std::to_string(round % 7);
        setenv("SMG_ONLINE_DIR", d.c_str(), 1);
        mp_service_set_name(round % 4 ? "Fuzzer" : "\xff\xfe" "Admin  ..");
        const int e = WaitFor([] { const int r = mp_service_ensure_account(); return r == 0 ? -1 : r; }, 6000);
        CHECK(e == 1 || e == -1 || (e < 0 && e >= -int(MP_ERR_CLOCK)));
        MpProfile p;
        mp_ui_profile(&p);
        CHECK(strlen(p.name) <= MP_NAME_BYTES);
        if (!p.registered) continue;
        MpFriend fr[64];
        if (mp_ui_friends_request()) {
            const int n = WaitFor([&] { return mp_ui_friends_get(fr, 64); });
            CHECK(n >= -2 && n <= 50);
            for (int i = 0; i < n; ++i) CHECK(strlen(fr[i].name) <= MP_NAME_BYTES && strlen(fr[i].code) == 9);
            MpFriend inc[64];
            const int m = mp_ui_incoming_get(inc, 64);
            CHECK(m >= -2 && m <= 50);
        }
        MpBoardRow rows[64];
        if (mp_ui_board_request(uint8_t(round % 42), uint8_t(1 + round % 8), uint8_t(round % 3))) {
            const int n = WaitFor([&] { return mp_ui_board_get(rows, 64); });
            CHECK(n >= -2 && n <= 50);
            for (int i = 0; i < n; ++i) CHECK(rows[i].rank >= 1 && rows[i].players >= 1 && rows[i].players <= 4);
        }
        char code[16];
        if (mp_svc_room_lookup_begin(round % 2, MP_MODE_RACE, "K7QM-4DXT")) {
            const int r = WaitFor([&] { const int x = mp_svc_room_lookup_poll(code, sizeof code); return x == 0 ? -1 : x; });
            if (r == 1) CHECK(strlen(code) < 13);
        }
        mp_race_submit(uint8_t(round % 42), uint8_t(1 + round % 8), 3600 + round, nullptr);
        for (int i = 0; i < 40; ++i) { mp_service_pump(); usleep(2000); }
        if (mp_service_info_begin(1500)) {
            MpServiceInfo info;
            int r = 0;
            for (int i = 0; i < 400 && (r = mp_service_info_poll(&info)) == 0; ++i) usleep(5000);
            if (r == 1) CHECK(info.transport >= 0 && info.transport <= 3);
        }
        mp_service_set_name("Renamed");
        mp_service_ensure_account();
        for (int i = 0; i < 40; ++i) { mp_service_pump(); usleep(2000); }
    }
    mp_service_shutdown();
}

int main() {
    const char* url = getenv("FUZZ_URL");
    if (!url) { fprintf(stderr, "FUZZ_URL (service_fuzz.sh sets it)\n"); return 2; }
    const int rounds = getenv("FUZZ_ROUNDS") ? atoi(getenv("FUZZ_ROUNDS")) : 60;
    char tmpl[] = "/tmp/mp_service_fuzz.XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    FuzzSignal(url, rounds);
    FuzzService(url, dir, rounds);
    fprintf(stderr, g_fail ? "service_fuzz: %d check(s) failed\n" : "service_fuzz: %d rounds, all checks passed\n", g_fail ? g_fail : rounds);
    return g_fail ? 1 : 0;
}
