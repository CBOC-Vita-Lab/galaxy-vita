// vram30: GXV_VRAM_GUARD's queue and reference check (platform/vgx/src/vgx_vramguard.h), pure logic.
//   nice g++ -std=c++17 -Iplatform/include -Iplatform/vgx/src tests/vgx_vramguard.cpp -o /tmp/vgx_vramguard && /tmp/vgx_vramguard
#include "vgx_vramguard.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace vgx::vramguard;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

struct Rig {
    Guard g;
    std::vector<std::string> log;
    std::vector<std::pair<int, uint32_t>> released;
    std::vector<std::pair<int, uint32_t>> held; // (kind, name) the "back end" still references
    int finishes = 0;
    bool finishOk = true;
    int releasesAtFinish = 0;
    Rig(unsigned defer = 3, unsigned giveUp = 600) { g.Configure(defer, giveUp); }
    void Req(uint8_t k, uint32_t n, const char* site, uint64_t present, uint32_t kib = 100) {
        g.Request(k, n, kib, site, present, [&](const char* s) { log.push_back(s); });
    }
    void Drain(uint64_t present) {
        g.Drain(present,
                [&](uint8_t k, uint32_t n) -> const char* {
                    for (auto& h : held) if (h.first == k && h.second == n) return "the bound framebuffer";
                    return nullptr;
                },
                [&]() { ++finishes; releasesAtFinish = static_cast<int>(released.size()); return finishOk; },
                [&](uint8_t k, uint32_t n) { released.push_back({k, n}); },
                [&](const char* s) { log.push_back(s); });
    }
};

int main() {
    { // deferred by 3 presents, and only after a GPU finish
        Rig r;
        r.Req(kTex, 10, "siteA", 100);
        r.Drain(100); r.Drain(101); r.Drain(102);
        CHECK(r.released.empty());
        CHECK(r.finishes == 0); // (nothing due: no finish)
        r.Drain(103);
        CHECK(r.released.size() == 1 && r.released[0].second == 10);
        CHECK(r.finishes == 1);
        CHECK(r.releasesAtFinish == 0); // the finish came before the release
        CHECK(r.g.Waiting() == 0);
    }
    { // one finish for all the names that are due in a present; a later request waits its own 3 presents
        Rig r;
        r.Req(kTex, 1, "a", 10); r.Req(kFbo, 2, "b", 10); r.Req(kRb, 3, "c", 10); r.Req(kTex, 4, "a", 12);
        r.Drain(13);
        CHECK(r.released.size() == 3);
        CHECK(r.finishes == 1);
        CHECK(r.g.Waiting() == 1);
        r.Drain(14);
        CHECK(r.released.size() == 3);
        r.Drain(15);
        CHECK(r.released.size() == 4 && r.finishes == 2);
        // FIFO order of the release
        CHECK(r.released[0].second == 1 && r.released[1].second == 2 && r.released[2].second == 3 && r.released[3].second == 4);
    }
    { // a finish that does not complete (a scene is open): nothing is given back, nothing is lost, no postponement is counted
        Rig r;
        r.Req(kTex, 5, "a", 0);
        r.finishOk = false;
        r.Drain(5);
        CHECK(r.released.empty() && r.g.Waiting() == 1);
        CHECK(r.g.Counters().finishFailed == 1 && r.g.Counters().postponed == 0);
        r.finishOk = true;
        r.Drain(6);
        CHECK(r.released.size() == 1);
    }
    { // a referenced name is logged once per site and postponed; released when the reference is gone
        Rig r;
        r.held.push_back({kTex, 7});
        r.Req(kTex, 7, "DeleteGpuTex", 0);
        r.Req(kTex, 8, "DeleteGpuTex", 0);
        r.held.push_back({kTex, 8});
        r.Drain(3);
        CHECK(r.released.empty());
        CHECK(r.finishes == 0); // (no finish for names that cannot be given back)
        CHECK(r.g.Counters().postponed == 2);
        int lines = 0;
        for (auto& l : r.log) if (l.find("FREE WHILE REFERENCED") != std::string::npos) ++lines;
        CHECK(lines == 1); // once per site
        CHECK(r.log[0].find("texture 7") != std::string::npos && r.log[0].find("DeleteGpuTex") != std::string::npos && r.log[0].find("by the bound framebuffer") != std::string::npos);
        r.Drain(4); r.Drain(5);
        CHECK(r.released.empty());
        r.Drain(6); // postponed to 3 + 3
        CHECK(r.g.Counters().postponed == 4);
        r.held.clear();
        r.Drain(9);
        CHECK(r.released.size() == 2);
    }
    { // another site gets its own line
        Rig r;
        r.held.push_back({kTex, 1}); r.held.push_back({kFbo, 2});
        r.Req(kTex, 1, "siteX", 0); r.Req(kFbo, 2, "siteY", 0);
        r.Drain(3);
        int lines = 0;
        for (auto& l : r.log) if (l.find("FREE WHILE REFERENCED") != std::string::npos) ++lines;
        CHECK(lines == 2);
    }
    { // give up after N postponements
        Rig r(1, 3);
        r.held.push_back({kTex, 9});
        r.Req(kTex, 9, "s", 0);
        for (uint64_t p = 1; p < 20; ++p) r.Drain(p);
        CHECK(r.released.size() == 1);
        CHECK(r.g.Counters().gaveUp == 1);
        bool saw = false;
        for (auto& l : r.log) if (l.find("GAVE UP") != std::string::npos) saw = true;
        CHECK(saw);
    }
    { // double requests: while queued, and a framebuffer object freed again after it was freed
        Rig r;
        r.Req(kFbo, 20, "first", 0);
        r.Req(kFbo, 20, "second", 1);
        CHECK(r.g.Waiting() == 1 && r.g.Counters().doubleRequests == 1);
        r.Drain(3);
        CHECK(r.released.size() == 1);
        r.Req(kFbo, 20, "third", 4); // freed already, never generated since
        CHECK(r.g.Waiting() == 0 && r.g.Counters().doubleRequests == 2);
        r.g.NoteGen(kFbo, 20); // vitaGL handed the slot out again
        r.Req(kFbo, 20, "fourth", 5);
        CHECK(r.g.Waiting() == 1 && r.g.Counters().doubleRequests == 2);
        int lines = 0;
        for (auto& l : r.log) if (l.find("DOUBLE FREE") != std::string::npos) ++lines;
        CHECK(lines == 2); // (sites "second" and "third")
    }
    { // textures are not subject to the dead-name rule (vitaGL ignores a delete of an unassigned slot)
        Rig r;
        r.Req(kTex, 30, "a", 0);
        r.Drain(3);
        r.Req(kTex, 30, "b", 4);
        CHECK(r.g.Waiting() == 1 && r.g.Counters().doubleRequests == 0);
    }
    { // backlog accounting and the census line
        Rig r;
        r.Req(kTex, 1, "a", 0, 4000); r.Req(kTex, 2, "a", 0, 2000);
        CHECK(r.g.WaitingKiB() == 6000 && r.g.Counters().backlogKiBMax == 6000 && r.g.Counters().backlogMax == 2);
        r.Drain(3);
        CHECK(r.g.WaitingKiB() == 0 && r.g.Counters().backlogKiBMax == 6000);
        char line[600];
        CHECK(!r.g.Census(1000.0, line, sizeof line));  // (the first call starts the window)
        CHECK(!r.g.Census(4000.0, line, sizeof line));
        CHECK(r.g.Census(6100.0, line, sizeof line));
        std::string s = line;
        CHECK(s.find("vram-guard: 5.1 s: frees requested 2 (all deferred), given to vitaGL 2, postponed for a reference 0") == 0);
        CHECK(s.find("largest backlog 6000 KiB (2 names)") != std::string::npos);
        CHECK(s.find("GPU finishes 1 (0 not completed") != std::string::npos);
        CHECK(!r.g.Census(7000.0, line, sizeof line)); // the window restarted
        CHECK(r.g.Census(11200.0, line, sizeof line));
        CHECK(std::string(line).find("frees requested 0") != std::string::npos);
    }
    { // defer 0: due at the next drain of the same present
        Rig r(0);
        r.Req(kTex, 1, "a", 5);
        r.Drain(5);
        CHECK(r.released.size() == 1);
    }
    { // a request of name 0 or an unknown kind is ignored
        Rig r;
        r.Req(kTex, 0, "a", 0);
        CHECK(r.g.Waiting() == 0);
    }
    std::printf(g_fail ? "vgx_vramguard: %d FAILURES\n" : "vgx_vramguard: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
