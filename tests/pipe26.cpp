// pipe26 host test: the raw-FIFO pieces of port/pipe25.h (RawChain, MergeSerial, PassStore/ComparePass) and a model of the late draw.
//   pipe26_test [turns] [passes]      env: P26_NEG=unfenced  the pass reads the live state the next turn's logic writes: VERIFY must catch it
//                                          P26_SLEEP=1 widens the windows
// Model turn: the game thread writes A, then for each pass a token (snapshot of the state the pass reads; in verify mode the pass is also
// walked in place into a buffer of its own) and the bytes Bi into the chain; at the hand-over a worker splices (pass i, then Bi) into the
// stream while the game thread runs the NEXT turn's logic, which writes the live state. The stream must equal the serial turn's bytes.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include "port/pipe25.h"
using namespace port::pipe25;

static bool gSleep = false, gUnfenced = false;
static void Nap() { if (gSleep) std::this_thread::sleep_for(std::chrono::microseconds(20)); }
static int gFail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++gFail; } } while (0)

// ---- the model: a pass reads 16 words of "state" (matrices, material animation, ...) and emits bytes from them
static constexpr int kWords = 16;
static uint32_t gLive[kWords];
static void Logic(uint32_t turn) { for (int i = 0; i < kWords; ++i) { gLive[i] = turn * 1009u + i * 31u; if (i == 7) Nap(); } }
static void WalkPass(const uint32_t* st, uint32_t pass, std::vector<uint8_t>& out) {
    for (int i = 0; i < kWords; ++i) {
        uint32_t w = st[i] ^ (pass * 0x9E3779B9u);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&w);
        out.insert(out.end(), p, p + 4);
        if (i == 7) Nap();
    }
}
static void Open(std::vector<uint8_t>& s, uint32_t tag, uint32_t n) { for (uint32_t i = 0; i < n; ++i) { uint8_t b = static_cast<uint8_t>(tag + i); s.push_back(b); } }

// one turn, serial: A P1 B1 ... Pk Bk
static void Serial(uint32_t passes, std::vector<uint8_t>& s) {
    Open(s, 0xA0, 100);
    for (uint32_t i = 0; i < passes; ++i) { WalkPass(gLive, i, s); Open(s, static_cast<uint8_t>(i), 300 + i * 70); }
}

static void TestChain() {
    RawChain c;
    c.chunkBytes = 1000;                       // small chunks: rolls happen
    uint8_t* p = c.Open();
    std::vector<uint8_t> want[5];
    uint32_t v = 0;
    for (int b = 0; b < 5; ++b) {
        c.Mark(p);
        const int n = 700 * (b + 1) + 13;
        for (int i = 0; i < n; ++i) {
            if (p >= c.Limit()) p = c.Roll(p);
            *p++ = static_cast<uint8_t>(v++ * 7);
            want[b].push_back(static_cast<uint8_t>((v - 1) * 7));
        }
    }
    c.Close(p);
    CHECK(c.Marks() == 5);
    CHECK(c.Chunks() > 5);
    for (uint32_t b = 0; b < 5; ++b) {
        std::vector<uint8_t> got;
        c.CopyAfter(b, [&](const uint8_t* q, size_t n) { got.insert(got.end(), q, q + n); });
        CHECK(got == want[b]);
        CHECK(c.SizeAfter(b) == want[b].size());
    }
    c.Release();
    p = c.Open();                              // reuse after Release
    c.Mark(p);
    *p++ = 9;
    c.Close(p);
    std::vector<uint8_t> g;
    c.CopyAfter(0, [&](const uint8_t* q, size_t n) { g.insert(g.end(), q, q + n); });
    CHECK(g.size() == 1 && g[0] == 9);
}

static void TestMerge() {
    const size_t W = 6;
    // pass 0 writes w0 and w1; open lists after it write w1 (win) and w2; pass 1 writes w3; open lists after write nothing more
    std::vector<std::vector<uint32_t>> snap = {{1, 1, 1, 1, 1, 1}, {1, 5, 7, 1, 1, 1}}, end = {{9, 8, 1, 1, 1, 1}, {1, 5, 7, 4, 1, 1}};
    uint32_t fin[6] = {1, 5, 7, 1, 1, 1};
    // serial: pass0 -> w0=9,w1=8; lists -> w1=5,w2=7; pass1 -> w3=4  => {9,5,7,4,1,1}
    CHECK(MergeSerial(fin, snap, end, W) == 2);
    const uint32_t exp[6] = {9, 5, 7, 4, 1, 1};
    CHECK(!memcmp(fin, exp, sizeof exp));
    uint32_t none[6] = {0};
    CHECK(MergeSerial(none, {}, {}, W) == 0);
}

static void TestCompare() {
    VerifyStats st;
    PassRec r; r.id = 3; r.type = 1; r.xlu = true; r.length = 8; r.models = {{0, "a"}, {4, "b"}};
    uint8_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8}, b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(ComparePass(r, a, b, 8, st, 11));
    b[5] = 0;
    CHECK(!ComparePass(r, a, b, 8, st, 12));
    CHECK(st.mismatches == 1 && strstr(st.first, "picture 12") && strstr(st.first, "pass 3") && strstr(st.first, "model 'b'") && strstr(st.first, "translucent"));
    CHECK(!ComparePass(r, a, b, 7, st, 13));      // length differs
    CHECK(st.mismatches == 2 && strstr(st.first, "picture 12"));  // only the first is kept
    r.comparable = false;
    CHECK(ComparePass(r, a, b, 8, st, 14) && st.skipped == 1);
    Census c; c.pictures = 4; c.deferred = 4; c.tokens = 20;
    char line[1024];
    LogCensus(c, 2, st, line, sizeof line);
    CHECK(strstr(line, "pipe25: GXV_PIPE25=2") && strstr(line, "MISMATCHES"));
}

// the late draw over many turns. Returns the verify stats.
static VerifyStats RunModel(uint32_t turns, uint32_t passes) {
    VerifyStats vs;
    RawChain chain;
    chain.chunkBytes = 512;
    std::thread worker;
    std::vector<uint8_t> stream;            // the spliced stream of the turn the worker is on
    std::vector<uint8_t> ref;               // serial reference of that turn
    std::vector<std::vector<uint8_t>> mine; // verify: the passes as the game thread walked them in place
    std::vector<std::vector<uint32_t>> snaps;
    std::vector<uint8_t> a;                 // A of the turn (read by the worker)
    for (uint32_t t = 0; t < turns; ++t) {
        Logic(t);                            // this turn's logic (beside the previous turn's worker)
        if (worker.joinable()) worker.join(); // the join before calcAnim: the previous turn's stream is complete
        if (t > 0) {
            ++vs.pictures;
            // the whole stream against the serial reference, and the passes against the in-place walks
            if (stream != ref) { ++vs.mismatches; if (!vs.first[0]) snprintf(vs.first, sizeof vs.first, "stream of picture %u differs", t - 1); }
        }
        // ---- the draw of turn t (game thread): A, tokens, chain
        ref.clear();
        Serial(passes, ref);                 // (the reference; the live state is this turn's, not yet touched by the next logic)
        stream.clear();
        a.clear();
        Open(a, 0xA0, 100);
        uint8_t* put = chain.Open();
        snaps.assign(passes, std::vector<uint32_t>());
        mine.assign(passes, std::vector<uint8_t>());
        for (uint32_t i = 0; i < passes; ++i) {
            chain.Mark(put);
            snaps[i].assign(gLive, gLive + kWords);      // the fence: a snapshot at the token
            WalkPass(gLive, i, mine[i]);                 // verify: in place
            std::vector<uint8_t> b;
            Open(b, static_cast<uint8_t>(i), 300 + i * 70);
            for (uint8_t x : b) { if (put >= chain.Limit()) put = chain.Roll(put); *put++ = x; }
        }
        chain.Close(put);
        // ---- hand-over; the worker splices and verifies while the next turn's logic runs
        worker = std::thread([&, t, passes] {
            stream = a;
            for (uint32_t i = 0; i < passes; ++i) {
                std::vector<uint8_t> p;
                WalkPass(gUnfenced ? gLive : snaps[i].data(), i, p);   // the unfenced control reads the live state
                PassRec r; r.id = i; r.length = static_cast<uint32_t>(mine[i].size());
                ComparePass(r, mine[i].data(), p.data(), static_cast<uint32_t>(p.size()), vs, t);
                stream.insert(stream.end(), p.begin(), p.end());
                chain.CopyAfter(i, [&](const uint8_t* q, size_t n) { stream.insert(stream.end(), q, q + n); });
            }
        });
    }
    if (worker.joinable()) worker.join();
    return vs;
}

int main(int argc, char** argv) {
    const uint32_t turns = argc > 1 ? static_cast<uint32_t>(atoi(argv[1])) : 200;
    const uint32_t passes = argc > 2 ? static_cast<uint32_t>(atoi(argv[2])) : 6;
    gSleep = getenv("P26_SLEEP") != nullptr;
    const char* neg = getenv("P26_NEG");
    gUnfenced = neg && !strcmp(neg, "unfenced");
    Logic(0);
    if (!gUnfenced) { TestChain(); TestMerge(); TestCompare(); }
    VerifyStats vs = RunModel(turns, passes);
    printf("pipe26 model: %llu pictures, %llu passes compared, %llu mismatches%s%s\n", (unsigned long long)vs.pictures, (unsigned long long)vs.compared,
           (unsigned long long)vs.mismatches, vs.mismatches ? "; " : "", vs.first);
    if (gUnfenced) {
        printf("negative control unfenced: %s\n", vs.mismatches ? "caught (as it must)" : "NOT CAUGHT (test broken)");
        return vs.mismatches ? 0 : 1;
    }
    return gFail == 0 && vs.mismatches == 0 && vs.compared > 0 ? 0 : 1;
}
