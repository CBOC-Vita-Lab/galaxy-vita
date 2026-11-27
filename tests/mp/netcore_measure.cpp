// Galaxy 3.0 multiplayer: netcore measurements on the loopback hub (virtual clock). Built against a tree (mp-int or netpack-m, the
// latter also with -DMP_BUNDLE_HOLD_MS=0) to compare:
//   pose   n players (2..4) send STATE at 30 Hz (phases spread), pumps every 5 ms, LAN link (2 + 0..3 ms): per receiver the age of
//          each STATE when the game takes it (host's own / relayed by the host), and datagrams per second on a host -> guest link
//   snap   a guest joins a host with an 8 KB progress snapshot: ms from the join press to the snapshot being ready (clean, 20 % loss)
//   event  reliable events (KILL, tracked) guest -> host -> guest at 0 / 20 % loss: delivery latency p50 / p95 / max
// Usage: netcore_measure [pose|snap|event|all]
#include "core_test_util.h"

#include <algorithm>
#include <map>

#ifndef MP_BUNDLE_HOLD_MS
#define MP_BUNDLE_HOLD_MS -1   // (a tree before netpack: no bundles)
#endif

static std::vector<MpCore*> Session(MpLoopHub& hub, int n, uint32_t seed) {
    std::vector<MpCore*> all;
    const char* names[4] = {"Player", "Luigi", "Peach", "Toad"};
    for (int i = 0; i < n; i++) all.push_back(MakeCore(hub, (uint32_t)(i + 1), names[i], seed + (uint32_t)i * 7919u));
    MpHostParams hp = HostParams();
    all[0]->uiHost(&hp);
    WaitPhase(all, all[0], MP_UI_LOBBY, 2000);
    for (int i = 1; i < n; i++) {
        ScanJoin(all, all[i]);
        WaitPhase(all, all[i], MP_UI_LOBBY, 8000);
    }
    all[0]->uiStart();
    Step(all, 500);
    return all;
}
static double Pct(std::vector<double> v, double q) {
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[(size_t)std::min((double)v.size() - 1, q * (double)(v.size() - 1) + 0.5)];
}

static uint64_t g_dgHostToG1 = 0;
static void CountTap(void*, const MpAddr& from, const MpAddr& to, const void*, size_t) {
    if (from.host == 1 && to.host == 2) g_dgHostToG1++;
}

static void Pose() {
    for (int n = 2; n <= 4; n++) {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        hub.setImpairment(MpLoopImpairment{0, 0, 2, 3, 0}, 7);
        std::vector<MpCore*> all = Session(hub, n, 501);
        std::vector<double> own, relayed;
        g_dgHostToG1 = 0;
        hub.setTap(CountTap, 0);
        const uint64_t t0 = g_now;
        std::vector<uint64_t> next(n);
        for (int i = 0; i < n; i++) next[i] = t0 + (uint64_t)(i * 11);
        while (g_now - t0 < 10000) {
            for (int i = 0; i < n; i++) {
                if (g_now < next[i]) continue;
                next[i] += 33 + (next[i] % 3 == 0 ? 1 : 0);
                uint8_t p[51] = {0};
                memcpy(p + 8, &g_now, 8);
                p[16] = (uint8_t)i;
                all[i]->send(MP_MSG_STATE, p, sizeof p, false);
            }
            Step(all, 5, 5);   // (the helper thread pumps about every 5 ms; the game takes what arrived as often)
            for (int r = 0; r < n; r++) {
                MpMsg m;
                while (all[r]->poll(&m)) {
                    if (m.type != MP_MSG_STATE || m.len != 51) continue;
                    uint64_t at;
                    memcpy(&at, m.data + 8, 8);
                    const int origin = m.data[16];
                    if (r == 0) continue;   // (the host's own view of the guests: direct)
                    (origin == 0 ? own : relayed).push_back((double)(g_now - at));
                }
            }
        }
        hub.setTap(0, 0);
        printf("pose players=%d hold=%d: host->guest datagrams/s %.1f | age host-own p50 %.0f p95 %.0f max %.0f | relayed p50 %.0f p95 %.0f max %.0f ms\n",
               n, MP_BUNDLE_HOLD_MS + 0, (double)g_dgHostToG1 / 10.0, Pct(own, 0.5), Pct(own, 0.95), Pct(own, 1.0), Pct(relayed, 0.5),
               Pct(relayed, 0.95), Pct(relayed, 1.0));
        for (MpCore* c : all) delete c;
    }
}

static void Snap() {
    for (int lossI = 0; lossI < 2; lossI++) {
        std::vector<double> ms;
        int fails = 0;
        for (uint32_t seed = 0; seed < 10; seed++) {
            MpLoopHub hub;
            hub.setClock(VNow, 0);
            hub.setImpairment(MpLoopImpairment{lossI ? 0.20f : 0.0f, 0, 2, 3, 0}, 100 + seed);
            MpCore* h = MakeCore(hub, 1, "Player", 601 + seed);
            MpCore* g = MakeCore(hub, 2, "Luigi", 701 + seed);
            std::vector<MpCore*> all = {h, g};
            MpHostParams hp = HostParams();
            h->uiHost(&hp);
            WaitPhase(all, h, MP_UI_LOBBY, 2000);
            static uint8_t img[8192];
            for (size_t i = 0; i < sizeof img; i++) img[i] = (uint8_t)(i * 31 + seed);
            h->snapshotOffer(img, sizeof img);
            Step(all, 200);
            bool joined = false;
            for (int t = 0; t < 5000 && !joined; t += 10) {
                const int cnt = g->uiLocalScan(MP_CONN_LOOPBACK);
                MpLocalRoom room;
                if (cnt > 0 && g->uiLocalGet(0, &room)) joined = g->uiJoinLocal(MP_CONN_LOOPBACK, 0);
                if (!joined) Step(all, 10);
            }
            const uint64_t t0 = g_now;
            uint64_t done = 0;
            for (int t = 0; t < 40000 && !done; t += 5) {
                Step(all, 5);
                if (g->snapshotReady() == sizeof img) done = g_now - t0;
            }
            if (done) ms.push_back((double)done); else fails++;
            delete h;
            delete g;
        }
        printf("snap loss=%d%%: join press -> 8 KB snapshot ready: p50 %.0f max %.0f ms, %d of 10 never\n", lossI ? 20 : 0, Pct(ms, 0.5),
               Pct(ms, 1.0), fails);
    }
}

static void Event() {
    for (int lossI = 0; lossI < 2; lossI++) {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = Session(hub, 3, 801);
        hub.setImpairment(MpLoopImpairment{lossI ? 0.20f : 0.0f, 0, 2, 3, 0}, 9);
        std::vector<double> lat;
        const uint64_t t0 = g_now;
        uint32_t k = 0;
        while (g_now - t0 < 20000) {
            if ((g_now - t0) % 100 == 0) {
                uint8_t p[16] = {0};
                memcpy(p, &g_now, 8);
                memcpy(p + 8, &k, 4);
                k++;
                all[1]->send(MP_MSG_KILL, p, 12, true);
            }
            Step(all, 5);
            MpMsg m;
            while (all[2]->poll(&m)) {
                if (m.type != MP_MSG_KILL || m.len != 12) continue;
                uint64_t at;
                memcpy(&at, m.data, 8);
                lat.push_back((double)(g_now - at));
            }
            for (int i = 0; i < 2; i++) while (all[i]->poll(&m)) {}
        }
        printf("event loss=%d%%: guest -> host -> guest, %zu of %u: p50 %.0f p95 %.0f max %.0f ms\n", lossI ? 20 : 0, lat.size(), k,
               Pct(lat, 0.5), Pct(lat, 0.95), Pct(lat, 1.0));
        for (MpCore* c : all) delete c;
    }
}

int main(int argc, char** argv) {
    mp_log_set_sink(LogSink, true);
    (void)g_fail;   // (core_test_util.h)
    const char* what = argc > 1 ? argv[1] : "all";
    if (!strcmp(what, "pose") || !strcmp(what, "all")) Pose();
    if (!strcmp(what, "snap") || !strcmp(what, "all")) Snap();
    if (!strcmp(what, "event") || !strcmp(what, "all")) Event();
    return 0;
}
