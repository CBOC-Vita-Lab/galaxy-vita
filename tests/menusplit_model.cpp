// menutrim25: host test of vgx_menu_split.h (GXV_STAGEQ_MENUSPLIT): the record tags, the classes, and the menu tier's selection, including a miniature of the c76 case.
#include "../platform/vgx/src/vgx_menu_split.h"
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>
using namespace vgx::menusplit;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

struct Rec { uint64_t vs, fs; uint32_t lb; };
static uint32_t Lb(uint32_t layout, uint32_t blend, uint32_t tag) { return (layout << 13) | blend | tag; }

struct Run {
    std::unordered_set<uint32_t> have;
    std::unordered_set<uint32_t> pkg;
    std::unordered_map<uint32_t, uint8_t> dropped;
    std::vector<uint32_t> taken;
    Census c;
    unsigned go(const std::vector<Rec>& r, bool split) {
        return Select(r.data(), r.size(), split,
            [](const Rec& x, uint32_t& k) { if (x.vs == 0) return false; k = static_cast<uint32_t>(x.vs << 16 | x.fs); return true; },
            [&](const Rec& x) { return pkg.count(static_cast<uint32_t>(x.vs << 16 | x.fs)) != 0; },
            [&](uint32_t k) { return have.count(k) != 0; },
            [&](const Rec&, uint32_t k) { have.insert(k); taken.push_back(k); },
            [&](uint32_t k, Class cl) { dropped.emplace(k, cl); }, c);
    }
};

int main() {
    // tags: the queue's reads of the word (layout bits 13..24, blend bits 0..12) ignore them
    {
        const uint32_t lb = Lb(0xABC, 0x1555, kTagPlay2D);
        CHECK(((lb >> 13) & 0xFFFu) == 0xABCu);
        CHECK((lb & 0x1FFFu) == 0x1555u);
        CHECK(Lb(0xFFF, 0x1FFF, 0) < (1u << 25));
        CHECK(ClassOf(Lb(1, 1, kTagPlay2D), false) == kPlay2D);
        CHECK(ClassOf(Lb(1, 1, kTagScene), true) == kScene);   // a tag wins over the package
        CHECK(ClassOf(Lb(1, 1, 0), true) == kPkg);
        CHECK(ClassOf(Lb(1, 1, 0), false) == kLegacy);
        CHECK(ClassOf(0x80000000u, false) == kLegacy);         // (a stray high bit is not a tag)
    }
    // recording: 3D in play and in the pause menu is not recorded; 2D is, everywhere but in the menu scenes, where everything is, as scene
    {
        Ctx play; CHECK(RecordTag(play, false) == 0); CHECK(RecordTag(play, true) == kTagPlay2D);
        Ctx pause; pause.menuRec = true; CHECK(RecordTag(pause, false) == 0); CHECK(RecordTag(pause, true) == kTagPlay2D);
        Ctx title; title.menuRec = true; title.noStage = true; CHECK(RecordTag(title, false) == kTagScene); CHECK(RecordTag(title, true) == kTagScene);
        Ctx sel; sel.menuRec = true; sel.layerRecent = true; CHECK(RecordTag(sel, false) == kTagScene); CHECK(RecordTag(sel, true) == kTagScene);
        Ctx fs; fs.fileSelect = true; CHECK(RecordTag(fs, false) == kTagScene); CHECK(RecordTag(fs, true) == kTagScene);
        Ctx noRec; noRec.layerRecent = true; /* menuRec false: the layer mark alone does not make a scene context */ CHECK(RecordTag(noRec, false) == 0);
    }
    // selection: off = every program; on = the always set only; programs the queue already holds are not taken nor counted as dropped
    {
        std::vector<Rec> r = {
            {1, 1, Lb(1, 1, kTagPlay2D)}, {1, 1, Lb(2, 1, kTagPlay2D)},   // same program twice
            {2, 1, Lb(1, 1, kTagScene)}, {3, 1, Lb(1, 1, 0)}, {4, 1, Lb(1, 1, 0)}, {5, 1, Lb(1, 1, 0)}, {0, 0, 0},
            {6, 1, Lb(1, 1, kTagScene)}, {6, 1, Lb(1, 1, kTagPlay2D)},     // scene first, then 2D: taken (the 2D record admits it)
        };
        Run off; off.pkg.insert(4u << 16 | 1u);
        CHECK(off.go(r, false) == 6); CHECK(off.dropped.empty()); CHECK(off.c.unknown == 1);
        Run on; on.pkg.insert(4u << 16 | 1u); on.have.insert(5u << 16 | 1u); // program 5 is the stage's own already
        const unsigned t = on.go(r, true);
        CHECK(t == 3);                                                      // 1 (2D), 4 (package), 6 (2D record)
        CHECK(on.c.kept[kPlay2D] == 2 && on.c.kept[kPkg] == 1 && on.c.kept[kScene] == 0 && on.c.kept[kLegacy] == 0);
        CHECK(on.dropped.size() == 3 && on.dropped.count(2u << 16 | 1u) && on.dropped.count(3u << 16 | 1u) && on.dropped.count(6u << 16 | 1u));
        CHECK(!on.dropped.count(5u << 16 | 1u));
        CHECK(on.dropped[2u << 16 | 1u] == kScene && on.dropped[3u << 16 | 1u] == kLegacy);
        CHECK(on.c.recs[kPlay2D] == 3 && on.c.recs[kScene] == 2 && on.c.recs[kLegacy] == 3 + 0 && on.c.recs[kPkg] == 1);
    }
    // the c76 case in miniature: own 263 + player 80 + a 968-record menu list (of 660 new programs) + shared; 1,024 slots, 48 reserved
    {
        const unsigned own = 263, player = 80, shared = 90, menuProgs = 660, slots = 1024 - 48;
        std::vector<Rec> menu;
        for (unsigned i = 0; i < 968; ++i) menu.push_back({1000 + (i % menuProgs), 1, Lb(i & 0xFFF, 1, 0)}); // all legacy
        for (unsigned i = 0; i < 100; ++i) menu.push_back({5000 + i, 1, Lb(1, 1, kTagPlay2D)});             // the 2D set recorded since
        for (unsigned i = 0; i < 76; ++i) menu.push_back({1000 + i, 1, Lb(7, 1, 0)});                       // the package's (some are in the legacy part too)
        Run old, now;
        for (Run* q : {&old, &now}) {
            for (unsigned i = 0; i < own + player; ++i) q->have.insert(i);
            for (unsigned i = 0; i < 76; ++i) q->pkg.insert(((1000 + i) << 16) | 1u);
        }
        old.go(menu, false); now.go(menu, true);
        CHECK(old.have.size() + shared > slots);          // the old queue does not fit
        CHECK(now.have.size() + shared <= 800);           // the new one does, with margin
        CHECK(now.have.size() == own + player + 100 + 76);
        CHECK(now.taken.size() == 176);
        unsigned legacyDropped = 0;
        for (const auto& kv : now.dropped) legacyDropped += kv.second == kLegacy;
        CHECK(legacyDropped == menuProgs - 76);
    }
    {   // (c183fix F5) records whose shaders are not known yet: offered for late resolution when the split would keep them, never when it would leave them out
        const std::vector<Rec> recs = {
            {0, 1, Lb(1, 1, kTagPlay2D)},   // unknown, 2D: always kept
            {0, 2, Lb(1, 2, kTagScene)},    // unknown, scene: left out with the split
            {0, 3, Lb(1, 3, 0)},            // unknown, legacy (not in the package): left out with the split
            {5, 4, Lb(1, 4, 0)},            // known
        };
        for (bool split : {false, true}) {
            std::vector<uint64_t> missing;
            Census c;
            std::unordered_set<uint32_t> have;
            Select(recs.data(), recs.size(), split,
                [](const Rec& x, uint32_t& k) { if (x.vs == 0) return false; k = static_cast<uint32_t>(x.vs << 16 | x.fs); return true; },
                [](const Rec&) { return false; }, [&](uint32_t k) { return have.count(k) != 0; },
                [&](const Rec&, uint32_t k) { have.insert(k); }, [](uint32_t, Class) {},
                [&](const Rec& x) { missing.push_back(x.fs); }, c);
            CHECK(c.unknown == 3);
            CHECK(split ? missing == std::vector<uint64_t>{1} : missing == (std::vector<uint64_t>{1, 2, 3}));
            CHECK(have.size() == (split ? 0u : 1u));   // (the known one is legacy: left out with the split)
        }
    }
    if (fails) { std::printf("%d failures\n", fails); return 1; }
    std::printf("menusplit_model: ok\n");
    return 0;
}
