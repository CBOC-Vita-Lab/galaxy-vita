// hrw27 (GXV_HRW stages d-h): host test of the partial draw's merge (vgx_hrw.h Compose / Uncompose: owner removal and insertion order, arena rebasing, byte-exact undo, refusals), the
// sky's rotation-only plan, the verify-fresh probes, and the significance set (platform/include/port/hrw_set.h: budget, stagger, protection, minimum score).
// Negative controls: the comparison must catch a deliberately wrong fresh draw; the undo hash must catch a deliberately left-over byte.
#include "../platform/vgx/src/vgx_hrw.h"
#include "../platform/include/port/hrw_set.h"
#include <cstdio>
#include <cstdlib>
using namespace vgx;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void persp(float p[16], float scale) {
    std::memset(p, 0, 16 * sizeof(float));
    p[0] = 1.2f * scale; p[5] = -1.7f * scale; p[10] = -1.01f; p[11] = -1.0f; p[14] = -2.0f;
}
// one draw: its own projection block and model-view block (appended to uni), `n` vertices of 16 bytes, 3 indices
static uint32_t PushBlock(CmdBuf& b, const float* v, size_t n) {
    const uint32_t off = static_cast<uint32_t>((b.uni.size() + 3) / 4 * 4);
    b.uni.resize(off + n);
    std::memcpy(b.uni.data() + off, v, n * sizeof(float));
    return off;
}
static void AddDraw(CmdBuf& b, uint16_t owner, float tag, bool chunked = true, uint32_t tex = 0) {
    float P[16]; persp(P, 1.0f);
    float mv[24] = {1, 0, 0, tag, 0, 1, 0, 0, 0, 0, 1, -10};
    DrawRec d;
    d.owner = owner;
    d.ublock[UB_PROJ] = PushBlock(b, P, 16);
    d.ublock[UB_MV] = PushBlock(b, mv, 24);
    d.vbuf = 0;
    if (chunked && b.vtxChunks.empty()) b.vtxChunks.push_back(0);
    d.vchunk = 0;
    d.vtxFirst = static_cast<uint32_t>(b.vtx.size() / 16);
    d.vtxCount = 4;
    d.idxOffset = static_cast<uint32_t>(b.idx.size());
    d.idxCount = 3;
    d.tex[0].texId = tex;
    const size_t v0 = b.vtx.size();
    b.vtx.resize(v0 + 64);
    for (size_t i = 0; i < 64; ++i) b.vtx[v0 + i] = static_cast<uint8_t>(tag + i);
    for (uint16_t i = 0; i < 3; ++i) b.idx.push_back(static_cast<uint16_t>(d.vtxFirst + i));
    b.draws.push_back(d);
    b.cmds.push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(b.draws.size() - 1)});
}
static CmdBuf* MakeBuf() {
    CmdBuf* b = new CmdBuf();
    b->cmds.reserve(256); b->draws.reserve(64); b->vtx.reserve(1 << 16); b->idx.reserve(1 << 14); b->uni.reserve(1 << 14);
    return b;
}
static float TagOf(const CmdBuf& b, const Cmd& c) { return b.uni[b.draws[c.arg].ublock[UB_MV] + 3]; }

int main() {
    CHECK(sizeof(DrawRec) == 232);   // (layout pin, host x86-64; was 200 before later stages grew the record)
    // ---- owner keys
    CHECK(hrw::OwnerKey(hrw::kOwnMario, hrw::kPhPlayerDecoXlu) == 0x0103 && hrw::OwnerId(0x0103) == 1 && hrw::OwnerPhaseOf(0x0103) == 3);
    CHECK(hrw::ParseMode("4") == hrw::kVerifyFresh && hrw::ParseMode("5") == hrw::kWarp && hrw::ParseMode("6") == 0);   // (hrw31: mode 5 exists)
    const uint16_t kDeco = hrw::OwnerKey(hrw::kOwnMario, hrw::kPhPlayerDecoOpa), kBody = hrw::OwnerKey(hrw::kOwnMario, hrw::kPhPlayerDraw), kXlu = hrw::OwnerKey(hrw::kOwnMario, hrw::kPhPlayerDecoXlu);
    const uint16_t kSky = hrw::OwnerKey(hrw::kOwnSky, hrw::kPhSky), kSet = hrw::OwnerKey(20, hrw::kPhGroupOpa + 3);

    // ---- Compose: removal at the anchor, insertion order, rebasing, exact undo
    CmdBuf* A = MakeBuf();
    AddDraw(*A, 0, 1);           // 0 world
    AddDraw(*A, kDeco, 2);       // 1 Mario deco (anchor of kDeco)
    AddDraw(*A, 0, 3);           // 2 world
    AddDraw(*A, kBody, 4);       // 3 Mario body (anchor)
    AddDraw(*A, kBody, 5);       // 4 Mario body again (removed, no insertion)
    AddDraw(*A, kSet, 6);        // 5 a set member: F has none of it, it stays
    AddDraw(*A, 0, 7);           // 6 world
    AddDraw(*A, kXlu, 8);        // 7 xlu deco (anchor)
    CmdBuf* F = MakeBuf();
    AddDraw(*F, kBody, 104);
    AddDraw(*F, kDeco, 102);
    AddDraw(*F, kBody, 105);
    AddDraw(*F, hrw::OwnerKey(30, 0x20), 130);   // an owner A has not got: left out, counted
    AddDraw(*F, 0, 199);                         // not owned: left out
    const uint64_t h0 = hrw::HashBuf(*A);
    hrw::Undo u; hrw::ComposeStats st; hrw::Scratch sc;
    int rc = hrw::Compose(*A, *F, u, st, sc);
    CHECK(rc == hrw::kComposeOk);
    CHECK(st.freshKeys == 3 && st.matchedKeys == 2 && st.orphanKeys == 1);
    CHECK(st.removed == 3 && st.inserted == 3 && st.orphanDraws == 1 && st.unownedFresh == 1);
    {
        // the composite: world(1) deco'(102) world(3) body'(104,105) [4 removed] set(6) world(7) xlu(8: not in F: stays)
        float want[] = {1, 102, 3, 104, 105, 6, 7, 8};
        size_t i = 0;
        for (const Cmd& c : A->cmds) { CHECK(i < 8 && TagOf(*A, c) == want[i]); ++i; }
        CHECK(i == 8);
        // rebased: a fresh draw's vertices, indices and blocks are F's data
        const Cmd& cb = A->cmds[3];
        const DrawRec& d = A->draws[cb.arg];
        const size_t cs = A->vtxChunks[d.vchunk];
        CHECK(A->vtx[cs + d.vtxFirst * 16] == static_cast<uint8_t>(104));
        CHECK(A->idx[d.idxOffset] == d.vtxFirst);
        float P[16]; persp(P, 1.0f);
        CHECK(std::memcmp(A->uni.data() + d.ublock[UB_PROJ], P, sizeof P) == 0);
        CHECK(A->uni[d.ublock[UB_MV] + 3] == 104.0f);
        // the retained draws not removed are untouched
        CHECK(A->draws[A->cmds[5].arg].owner == kSet && A->vtxChunks.size() >= 2);
    }
    hrw::Uncompose(*A, u);
    CHECK(hrw::HashBuf(*A) == h0 && !u.active);
    // negative control: a byte left behind must change the hash
    rc = hrw::Compose(*A, *F, u, st, sc); CHECK(rc == hrw::kComposeOk);
    hrw::Uncompose(*A, u);
    A->vtx.push_back(1);
    CHECK(hrw::HashBuf(*A) != h0);
    A->vtx.pop_back();
    CHECK(hrw::HashBuf(*A) == h0);
    // compose again and again, different F each time: always back to the same bytes
    for (int k = 0; k < 20; ++k) {
        rc = hrw::Compose(*A, *F, u, st, sc); CHECK(rc == hrw::kComposeOk);
        hrw::Uncompose(*A, u); CHECK(hrw::HashBuf(*A) == h0);
    }
    // ---- refusals
    { CmdBuf* E = MakeBuf(); CHECK(hrw::Compose(*A, *E, u, st, sc) == hrw::kNoFresh); CHECK(hrw::HashBuf(*A) == h0); delete E; }
    { CmdBuf* G = MakeBuf(); AddDraw(*G, hrw::OwnerKey(77, 0x20), 1); CHECK(hrw::Compose(*A, *G, u, st, sc) == hrw::kNoAnchor); CHECK(hrw::HashBuf(*A) == h0); delete G; }
    { // no room: a base whose arenas are exactly full
        CmdBuf* T = MakeBuf(); AddDraw(*T, kBody, 1); T->vtx.shrink_to_fit(); T->idx.shrink_to_fit(); T->uni.shrink_to_fit();
        CmdBuf* G = MakeBuf(); AddDraw(*G, kBody, 2);
        CHECK(hrw::Compose(*T, *G, u, st, sc) == hrw::kNoRoom); delete T; delete G; }
    { // a fresh texture upload that A's draws sample
        CmdBuf* T = MakeBuf(); AddDraw(*T, kBody, 1, true, 42);
        CmdBuf* G = MakeBuf(); AddDraw(*G, kBody, 2); TexUpload up; up.texId = 42; G->uploads.push_back(up); G->cmds.insert(G->cmds.begin(), Cmd{CMD_TEX_UPLOAD, 0});
        CHECK(hrw::Compose(*T, *G, u, st, sc) == hrw::kHazardTex);
        G->uploads[0].texId = 43;  // another texture: fine, and the upload command stays out of the composite's draws
        CHECK(hrw::Compose(*T, *G, u, st, sc) == hrw::kComposeOk); hrw::Uncompose(*T, u);
        delete T; delete G; }
    { // chunk scheme mismatch
        CmdBuf* T = MakeBuf(); AddDraw(*T, kBody, 1, false);
        CmdBuf* G = MakeBuf(); AddDraw(*G, kBody, 2, true);
        CHECK(hrw::Compose(*T, *G, u, st, sc) == hrw::kChunkMismatch); delete T; delete G; }
    CHECK(std::strcmp(hrw::RefusalName(hrw::kNoRoom), "no room in the arenas") == 0 && std::strcmp(hrw::RefusalName(99), "?") == 0);

    // ---- the sky: rotation-only delta, restored byte for byte, a shared block counted
    {
        CmdBuf* S = MakeBuf();
        AddDraw(*S, kSky, 1);
        AddDraw(*S, 0, 2);
        const uint64_t hs = hrw::HashBuf(*S);
        hrw::Plan pl;
        CHECK(pl.Collect(*S) && pl.sky == 1 && pl.refs.size() == 1 && pl.skyRefs.size() == 1 && !pl.conflict);
        const float cy = std::cos(0.05f), sy = std::sin(0.05f);
        const float dd[12] = {cy, 0, sy, 30, 0, 1, 0, 0, -sy, 0, cy, 40};  // a turn and a translation
        pl.Apply(*S, dd);
        float rotd[12]; hrw::RotOnly(dd, rotd);
        CHECK(rotd[3] == 0 && rotd[7] == 0 && rotd[11] == 0 && rotd[0] == cy);
        float P[16], out1[16], out2[16]; persp(P, 1.0f);
        hrw::ProjMulD(P, rotd, out1); hrw::ProjMulD(P, dd, out2);
        CHECK(std::memcmp(S->uni.data() + S->draws[0].ublock[UB_PROJ], out1, sizeof out1) == 0);   // sky: rotation only
        CHECK(std::memcmp(S->uni.data() + S->draws[1].ublock[UB_PROJ], out2, sizeof out2) == 0);   // world: all of it
        CHECK(std::memcmp(out1, out2, sizeof out1) != 0);
        pl.Restore(*S); CHECK(hrw::HashBuf(*S) == hs);
        // a sky draw sharing its block with a world draw: no sky refs, counted
        S->draws[1].ublock[UB_PROJ] = S->draws[0].ublock[UB_PROJ];
        CHECK(pl.Collect(*S) && pl.skyRefs.empty() && pl.skyShared == 1 && !pl.conflict);
        delete S;
    }

    // ---- verify-fresh probes: the same draws with the same camera compare as 0; a wrong fresh draw is caught
    {
        CmdBuf* X = MakeBuf(); AddDraw(*X, kBody, 3); AddDraw(*X, kDeco, 4); AddDraw(*X, 0, 5);
        CmdBuf* Y = MakeBuf(); AddDraw(*Y, kDeco, 4); AddDraw(*Y, kBody, 3);
        hrw::OwnerProbes pa, pb;
        hrw::CollectProbes(*X, pa); hrw::CollectProbes(*Y, pb);
        CHECK(pa.v.size() == 2 && pb.v.size() == 2);
        hrw::CompareResult r = hrw::CompareProbes(pa, pb);
        CHECK(r.keys == 2 && r.keysBadCount == 0 && r.drawsCompared == 2 && r.drawsBad == 0 && r.maxPx < 1e-4);
        CmdBuf* Z = MakeBuf(); AddDraw(*Z, kDeco, 4); AddDraw(*Z, kBody, 9);  // Mario's body 6 units away
        hrw::OwnerProbes pc; hrw::CollectProbes(*Z, pc);
        r = hrw::CompareProbes(pa, pc);
        CHECK(r.drawsBad == 1 && r.maxPx > 0.25);
        CmdBuf* W = MakeBuf(); AddDraw(*W, kDeco, 4);  // a draw missing
        hrw::OwnerProbes pd; hrw::CollectProbes(*W, pd);
        r = hrw::CompareProbes(pa, pd);
        CHECK(r.keysBadCount == 1);
        delete X; delete Y; delete Z; delete W;
    }

    // ---- the significance set
    {
        using namespace port::hrwset;
        Params p;
        std::vector<Cand> c;
        auto mk = [&](uint32_t id, float x, float z, float vx, float rad, float nearD, float cost, uint8_t fl = 0, uint8_t stale = 0) {
            Cand k; k.id = id; k.eye[0] = x; k.eye[1] = 0; k.eye[2] = -z; k.vel[0] = vx; k.radius = rad; k.nearDist = nearD; k.costMs = cost; k.flags = fl; k.stale = stale; c.push_back(k);
        };
        mk(0, 0, 400, 0, 80, 0, 0.9f, kProtected);       // Mario: protected
        mk(1, 50, 600, 0, 60, 300, 0.4f);                // near and large
        mk(2, 0, 20000, 0, 50, 15000, 0.3f);             // far, small, still
        mk(3, 200, 3000, 60, 200, 3000, 0.5f);           // fast and big
        mk(4, 0, 3000, 0, 20, 3000, 0.2f);               // small still, not near
        mk(5, 0, 700, 0, 50, 500, 0.6f);                 // near, medium
        int out[32]; double used = 0; int worth = 0;
        p.budgetMs = 5.0;
        int n = Select(c.data(), static_cast<int>(c.size()), p, out, 32, &used, &worth);
        CHECK(n >= 3 && out[0] == 0);                        // Mario first, whatever the budget
        bool in2 = false; for (int i = 0; i < n; ++i) if (out[i] == 2) in2 = true;
        CHECK(!in2);                                          // far, small and still is never refreshed
        CHECK(used <= 5.0 + 1e-9);
        // budget: with 0.5 ms only the most significant that fit are taken; Mario is not charged
        p.budgetMs = 0.5; n = Select(c.data(), static_cast<int>(c.size()), p, out, 32, &used, &worth);
        CHECK(out[0] == 0 && used <= 0.5 + 1e-9 && n >= 2);
        // 0 budget: Mario alone
        p.budgetMs = 0.0; n = Select(c.data(), static_cast<int>(c.size()), p, out, 32, &used, &worth); CHECK(n == 1 && out[0] == 0 && used == 0.0);
        // ordering: most significant first (score of the order, ties by index)
        p.budgetMs = 100; n = Select(c.data(), static_cast<int>(c.size()), p, out, 32, &used, &worth);
        for (int i = 2; i < n; ++i) CHECK(Score(c[out[i - 1]], p).score + 0.75f * c[out[i - 1]].stale >= Score(c[out[i]], p).score + 0.75f * c[out[i]].stale - 1e-5f);
        // stagger: two equally significant actors, room for one: the one that waited goes first, and over pairs both are served
        std::vector<Cand> t;
        { Cand a; a.id = 10; a.eye[2] = -600; a.radius = 60; a.nearDist = 300; a.costMs = 1.0f; t.push_back(a); a.id = 11; t.push_back(a); }
        p.budgetMs = 1.0;
        int served[2] = {0, 0}; uint8_t waited[2] = {0, 0};
        for (int pair = 0; pair < 20; ++pair) {
            t[0].stale = waited[0]; t[1].stale = waited[1];
            n = Select(t.data(), 2, p, out, 8, &used, &worth);
            CHECK(n == 1);
            for (int i = 0; i < 2; ++i) { if (out[0] == i) { ++served[i]; waited[i] = 0; } else if (waited[i] < 255) ++waited[i]; }
        }
        CHECK(served[0] == 10 && served[1] == 10);
        // too dear for what is left: the dear one is skipped, a cheaper one after it still goes in
        std::vector<Cand> q;
        { Cand a; a.id = 20; a.eye[2] = -600; a.radius = 60; a.nearDist = 100; a.costMs = 3.0f; q.push_back(a); Cand b = a; b.id = 21; b.nearDist = 400; b.costMs = 0.5f; q.push_back(b); }
        p.budgetMs = 1.0; n = Select(q.data(), 2, p, out, 8, &used, &worth); CHECK(n == 1 && out[0] == 1 && worth == 2);
        // maxN
        p.budgetMs = 100; p.maxN = 1; n = Select(c.data(), static_cast<int>(c.size()), p, out, 32, &used, &worth); CHECK(n == 2);
        // behind the camera counts as large; a speck that moves fast does not matter
        Cand sp; sp.eye[2] = -5000; sp.radius = 3; sp.vel[0] = 400; sp.nearDist = 5000;
        CHECK(Score(sp, Params()).score < Params().minScore);
        Cand bh; bh.eye[2] = 10; bh.radius = 10; CHECK(SizePx(bh, Params()) == Params().screenH);
    }
    delete A; delete F;
    if (fails) { std::printf("hrw27 test: %d FAILED\n", fails); return 1; }
    std::printf("hrw27 test: all passed\n");
    return 0;
}
