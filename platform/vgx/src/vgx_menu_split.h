// menutrim25 (GXV_STAGEQ_MENUSPLIT, default 0): what part of the menu list a stage's queue needs. Pure, standard library only (the backend and
// tests/menusplit_model.cpp share it).
//
// c76 (console, 12 Nov): the menu list (pseudo stage 0x4D454E55, kept on the card) held 968 records and added 600-680 programs to EVERY stage's queue, on
// top of the stage's own 200-400, the player list and the shared ones: 1,015-1,148 programs for 1,024 slots, so the queue stalled from the 4th stage of a
// session on and the stage's own programs were linked by draws in play (56 in a 22-stage tour). The list is not a menu list: it is recorded by
// StageNote while g_menuRec is on (no stage yet, the pause menu waits, or ScenarioSelectScene::draw marked the last 150 ms), and during the star
// select's white fade-in (the game scene's opening camera is drawn then, 1-2 s a stage) and the first two pictures of every pause (the whole frozen 3D
// scene, PETARI_PAUSE_FREEZE) that records the 3D world of every stage visited. 446 records on 10 Nov, 508 (c54-A), 817 (c54-B: one more tour), 931, 968.
//
// The split: every record of the menu list carries a class, in bits 26..27 of its layout/blend word (the queue reads only bits 0..24):
//   kPlay2D  an orthographic (2D layout) draw outside the title / file select / star select: the HUD, the pause menu, the talk windows, star get, miss,
//            game over. In every stage's queue ("always").
//   kScene   a draw while the title, the file select or the star select / galaxy map was up (any projection), and what is drawn behind its fade. Not in
//            a stage's queue: its scene is the stage that shows it (its own list holds them) or the start-up queue of the menu list itself.
//   kLegacy  a record made before the tags (the card's 968): unknown. Not in a stage's queue unless the package's own menu list holds it (kPkg): the
//            package's 76 are the title, file select, galaxy map and pause set recorded for the release. A pause menu that is then opened links a
//            program of the legacy set at most once: its draw records it again as kPlay2D.
// A 3D draw in play or in the pause menu is no longer recorded in the menu list at all (the stage's own list holds it).
#pragma once
#include <cstddef>
#include <cstdint>

namespace vgx::menusplit {

constexpr uint32_t kTagMask = 3u << 26;
constexpr uint32_t kTagPlay2D = 1u << 26;
constexpr uint32_t kTagScene = 2u << 26;
enum Class : uint8_t { kLegacy = 0, kPkg = 1, kPlay2D = 2, kScene = 3, kClassN = 4 };
inline const char* ClassName(unsigned c) {
    static const char* const n[kClassN] = {"legacy", "package", "2D", "scene"};
    return c < kClassN ? n[c] : "?";
}
inline Class ClassOf(uint32_t lb, bool inPackage) {
    const uint32_t t = lb & kTagMask;
    if (t == kTagPlay2D) return kPlay2D;
    if (t == kTagScene) return kScene;
    return inPackage ? kPkg : kLegacy;
}
// the "always" set: in every stage's queue
inline bool Always(Class c) { return c == kPlay2D || c == kPkg; }

// What the frame knows when a draw is recorded.
struct Ctx {
    bool menuRec = false;      // g_menuRec: no stage yet, the pause menu waits, or the star select / galaxy map drew in the last 150 ms
    bool noStage = false;      // no stage key (boot, title)
    bool layerRecent = false;  // ScenarioSelectScene::draw marked in the last 150 ms
    bool fileSelect = false;   // the stage is the title's file select
};
// The tag a draw gets in the menu list, 0 = it is not recorded there.
inline uint32_t RecordTag(const Ctx& c, bool ortho) {
    if (c.fileSelect || (c.menuRec && (c.noStage || c.layerRecent))) return kTagScene;
    if (ortho) return kTagPlay2D;
    return 0;
}

// The census of one queue build: the menu list's programs by class, those the queue took and those it left out.
struct Census {
    unsigned recs[kClassN] = {};    // records of the list by class
    unsigned kept[kClassN] = {};    // programs of the list the queue took (new to the queue)
    unsigned dropped[kClassN] = {}; // programs left out that the queue did not get from another list either (filled by the caller at the end)
    unsigned unknown = 0;           // records whose shaders this run does not know
};

// The menu tier of a queue: `recs` are the records (r.lb, and what Resolve needs), `Resolve(r, key)` the program key or false when its shaders are not known, `IsPkg(r)` whether
// the package's menu list holds the record's program, `Have(key)` whether the queue holds the program already, `Take(r, key)` puts it into the queue, `Drop(key, cls)` notes a
// program left out. With split off every record is taken (the old rule). Returns the programs taken.
// (c183fix F5) `Missing(r)`: a record whose shaders are not known yet and that the split would keep (split off, or an "always" class), so that the caller can resolve it
// later in the visit (the first-visit queue); a record the split would leave out is not offered.
template <class Rec, class Resolve, class IsPkg, class Have, class Take, class Drop, class Missing>
unsigned Select(const Rec* recs, size_t n, bool split, Resolve resolve, IsPkg isPkg, Have have, Take take, Drop drop, Missing missing, Census& c) {
    unsigned taken = 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t key = 0;
        const Class cls = ClassOf(static_cast<uint32_t>(recs[i].lb), isPkg(recs[i]));
        ++c.recs[cls];
        if (!resolve(recs[i], key)) {
            ++c.unknown;
            if (!split || Always(cls)) missing(recs[i]);
            continue;
        }
        if (split && !Always(cls)) {
            if (!have(key)) drop(key, cls);
            continue;
        }
        if (have(key)) continue;
        take(recs[i], key);
        ++c.kept[cls];
        ++taken;
    }
    return taken;
}
template <class Rec, class Resolve, class IsPkg, class Have, class Take, class Drop>
unsigned Select(const Rec* recs, size_t n, bool split, Resolve resolve, IsPkg isPkg, Have have, Take take, Drop drop, Census& c) {
    return Select(recs, n, split, resolve, isPkg, have, take, drop, [](const Rec&) {}, c);
}

}  // namespace vgx::menusplit
