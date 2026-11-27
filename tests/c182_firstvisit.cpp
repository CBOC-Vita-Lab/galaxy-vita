#include "vgx_stage_first_visit.h"
#include <cassert>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace vgx::firstvisit;
static void Cases() {
    Queue q;
    assert(q.First(0xffff, 0, 0x1ffffff));
    assert(!q.First(0xffff, 0, 0x1ffffff | slowTag));
    assert(!q.First(0xffff, 0, 0x1ffffff | scenarioMask | canDrawTag)); // (seedrun31: the hints are not part of the draw)
    assert(q.Seen(0xffff, 0, 0x1ffffff) && !q.Seen(0xfffe, 0, 0x1ffffff));
    assert(q.First(0xffff, 0, 0x1ffefff)); // distinct blend
    assert(q.First(0xffff, 0, 0xffefff));  // distinct layout
    assert(q.First(0, 0xffff, 0x1ffffff));
    q.Wait(0x10000000full, 21, {missing, missing, 5, 1});
    q.Wait(0x10000000full, 21, {missing, missing, slowTag | 5, 2});
    q.Wait(15, 29, {missing, 3, 6, 2});
    q.Register(false, 215, 13);
    q.Register(false, 21, 9); // wrong shader kind
    assert(!q.HasReady() && q.Waiting() == 3);
    q.Register(false, 0x20000000full + 15, 8); // (seedrun31: no low-32-bit links any more: a key whose low bits match is not the key)
    assert(!q.HasReady() && q.Waiting() == 3);
    q.Register(false, 15, 8);
    assert(q.HasReady() && q.Waiting() == 2);
    q.Register(false, 0x10000000full, 0xffff);
    q.Register(true, 21, missing); // invalid ID does not fulfill a promise
    assert(q.Waiting() == 2);
    q.Register(true, 21, 0);
    q.Register(true, 21, 0); // no duplicate notification
    std::vector<Variant> out;
    q.TakeReady([&](Variant v) { if (q.First(v.vs, v.fs, v.lb)) out.push_back(v); });
    assert(!q.HasReady() && !q.Waiting() && out.size() == 2);
    assert(out[0].vs == 0xffff && out[0].fs == 0 && out[0].tier == 1);
    assert(out[1].vs == 8 && out[1].fs == 3);
    q.Wait(123, 321, {missing, 0, 0, 1});
    q.Clear();
    q.Register(false, 123, 1);
    assert(!q.HasReady() && !q.Waiting() && q.First(0xffff, 0, 5));
    assert(LearnSlow(100, slowLimit - 1) && !LearnSlow(99.9, 0) && !LearnSlow(1000, slowLimit));
    {   // (seedrun31 A3) the record word's hints
        assert(ScenarioBits(0) == 0 && ScenarioBits(-1) == 0 && ScenarioBits(8) == 0 && ScenarioBits(1) == 1u << 29 && ScenarioBits(7) == 7u << 29);
        assert(ScenarioOf(0x16400 | ScenarioBits(5) | slowTag) == 5 && ScenarioOf(0x1ffffff | slowTag | canDrawTag | (3u << 26)) == 0);
        assert((drawMask & (canDrawTag | slowTag | scenarioMask | (3u << 26))) == 0 && (canDrawTag & (3u << 26)) == 0); // (no bit shared with the draw or menusplit)
        assert(ScenarioMatch(0x400, 3) && ScenarioMatch(0x400 | ScenarioBits(3), 3) && !ScenarioMatch(0x400 | ScenarioBits(2), 3) && ScenarioMatch(0x400 | ScenarioBits(2), 0));
        assert(Priority(0x400 | slowTag, 3) && Priority(0x400 | slowTag | ScenarioBits(3), 3) && !Priority(0x400 | slowTag | ScenarioBits(4), 3));
        assert(!Priority(0x400, 0) && !Priority(0x400 | slowTag | canDrawTag, 0) && Priority(0x400 | slowTag | ScenarioBits(4), 0));
        // the label of a new record: (cur, haveAny, haveCur, haveOther)
        assert(RecordLabel(0, false, false, false) == 0 && RecordLabel(0, true, false, false) == -1 && RecordLabel(0, false, false, true) == 0);
        assert(RecordLabel(3, false, false, false) == 3 && RecordLabel(3, false, true, false) == -1 && RecordLabel(3, true, false, true) == -1);
        assert(RecordLabel(3, false, false, true) == 0 && RecordLabel(9, false, false, false) == 0);
        // the own records' order
        struct R { uint32_t lb; };
        const std::vector<R> recs = {{1}, {2 | ScenarioBits(2)}, {3 | ScenarioBits(3)}, {4}, {5 | ScenarioBits(3)}, {6 | ScenarioBits(1)}};
        std::vector<uint32_t> order;
        assert(ScenarioFirst(recs, 3, order) == 4 && order == (std::vector<uint32_t>{0, 2, 3, 4, 1, 5}));
        assert(ScenarioFirst(recs, 0, order) == 6 && order == (std::vector<uint32_t>{0, 1, 2, 3, 4, 5})); // (not known: the record order)
        const std::vector<R> plain = {{1}, {2}, {3}};
        assert(ScenarioFirst(plain, 5, order) == 3 && order == (std::vector<uint32_t>{0, 1, 2})); // (no hints: the record order, whatever the scenario)
        // the head: unlinked first, then priority, both stable; nothing added or dropped
        std::deque<int> h = {1, 2, 3, 4, 5, 6, 7, 8};
        const size_t n = ArrangeHead(h, 6, true, true, [](int x) { return x % 2 == 0; }, [](int x) { return x == 4 || x == 3; });
        assert(n == 2 && h == (std::deque<int>{4, 3, 2, 6, 1, 5, 7, 8}));
        std::deque<int> h2 = {1, 2, 3};
        assert(ArrangeHead(h2, 9, false, false, [](int) { return true; }, [](int) { return true; }) == 0 && h2 == (std::deque<int>{1, 2, 3}));
        // the other scenarios' own records behind the player list: hot 1, own-cur 2, own-other 2, player 3, menu
        std::deque<int> o = {0, 10, 11, 20, 21, 30, 31, 32, 40};
        OthersBehindPlayer(o, 3, 2, 3);
        assert(o == (std::deque<int>{0, 10, 11, 30, 31, 32, 20, 21, 40}));
        OthersBehindPlayer(o, 3, 0, 3);
        assert(o == (std::deque<int>{0, 10, 11, 30, 31, 32, 20, 21, 40}));
    }
    {   // (seedrun31 A4) the can-draw tier's gate: behind the whole queue, covered frames only, cached shaders only
        CanDrawIn in;
        in.covered = true; in.cached = true;
        assert(CanDrawGate(in) == CanDrawStep::Take);
        in.queueEmpty = false; assert(CanDrawGate(in) == CanDrawStep::Wait); in.queueEmpty = true;
        in.live = true; assert(CanDrawGate(in) == CanDrawStep::Wait); in.live = false;          // never in play
        in.covered = false; assert(CanDrawGate(in) == CanDrawStep::Wait); in.covered = true;     // never a cover of its own
        in.cached = false; assert(CanDrawGate(in) == CanDrawStep::Skip);                         // never a compile for a guess
        in.linked = true; assert(CanDrawGate(in) == CanDrawStep::Take);                          // a new variant of a linked program
    }
    {   // (c183fix F5) late resolution of the menu tier: placement, the build's one-variant-a-program rule, and the tier it leaves
        using namespace vgx::stageroom;
        assert(LatePlace(kOwn, true) == Late::Front && LatePlace(kPlayer, false) == Late::Front);  // (C183's rule for own / player / seed)
        assert(LatePlace(kMenu, false) == Late::Back && LatePlace(kMenu, true) == Late::Skip);
        assert(LateTier(kOwn, kPlayer) == kOwn && LateTier(kOwn, kMenu) == kOwn);
        assert(LateTier(kMenu, kOwn) == kOwn && LateTier(kHot, kPlayer) == kPlayer && LateTier(kShared, kPlayer) == kPlayer);
        assert(LateTier(kPlayer, kMenu) == kPlayer && LateTier(kHot, kMenu) == kHot);  // a menu record never lowers a head program's protection
        assert(LateTier(kShared, kMenu) == kMenu && LateTier(kNone, kMenu) == kMenu && LateTier(kMenu, kMenu) == kMenu);
        assert(LateHolds(kOwn, false) && LateHolds(kPlayer, true) && !LateHolds(kPlayer, false) && !LateHolds(kMenu, true));
        // the queue: a menu record waits like the others and comes back with its tier
        Queue q;
        q.Wait(0x77, 0x88, {missing, missing, 9, kMenu});
        q.Register(false, 0x77, 4);
        q.Register(true, 0x88, 5);
        std::vector<Variant> got;
        q.TakeReady([&](Variant v) { got.push_back(v); });
        assert(got.size() == 1 && got[0].vs == 4 && got[0].fs == 5 && got[0].tier == kMenu);
    }
    std::puts("first-visit: exact variants, full-key registration, stage isolation, slow-learning limits, scenario hints, head order, can-draw gate PASS");
}

// ---- Replay (seedrun31 A3, seedrun31b): the queue a stage's load makes from a stage list, the rule until seedrun31 against the data rule ----
// "Before" = vgx_backend_gl.cpp / vgx_stage_first_visit.h until seedrun31: priority = the slow tag or the built-in Historical() hashes of the stage being built, and the
// built-in player seed (kPlySeed) queued after every player record, matched by the low 32 bits of the registered shaders' key hashes (both copied below as the
// reference, nowhere else). "Data" = vgx::firstvisit::Priority on the list made by tools/seedrun/stagetags.py legacy (the nine tagged, the seed as PLYR records at
// the end), no built-in anything. The model is StageBuild's: the stage's own records (deduplicated by draw), then the player list (and, before, the seed); hot / menu /
// shared programs left out (GXV_STAGEWARM_COMMON=0, GXV_STAGEWARM_MENU=0: they are the same lists either way); the head (own + player, GXV_PLAYERWARM_HOLD=1)
// rearranged: unlinked first, then the priority variants. Every 7th program is taken as linked already. The registry is every key hash of the list.
// Compared for EVERY stage of the list, and for three loads of the data list: scenario not known, scenario 3, and a card that holds the old untagged list with the
// tagged package's new records appended (what StageLoad + StageLoadPackaged make on a card that played the earlier VPK).
struct Rec { uint32_t stage, lb; uint64_t vs, fs; };
static_assert(sizeof(Rec) == 24);
static bool BeforeHistorical(uint32_t stage, uint64_t vs, uint64_t fs, uint32_t layoutBlend) {
    const auto lb = layoutBlend & 0x1ffffffu;
    if (stage == 0x4903d024u) {
        if (lb == 0x16c00u)
            return (vs == 0x671b7a9fae441691ull && (fs == 0x0d14f45bae134e25ull || fs == 0x85da8833ca90cc24ull)) ||
                   (vs == 0x1c119354708911ffull && (fs == 0x854159d5c0a23060ull || fs == 0xb0119fce9ea638a1ull));
        return (lb == 0x16400u && vs == 0x7ed555ea91ecd182ull && fs == 0x8ec5794f6b01cbafull) ||
               (lb == 0x16551u && vs == 0x1fb3fa05b81b6e91ull && fs == 0xec49bbef9d0c8383ull);
    }
    if (stage == 0x17bab031u)
        return (lb == 0x1c551u && vs == 0x166a6a6b5c0367f7ull && fs == 0x11c015bfcb16a96cull) ||
               (((lb == 0x1016400u && vs == 0xefb2f05617d51094ull) || (lb == 0x16400u && vs == 0xe94d45d78111dceeull)) &&
                fs == 0x6652cbb3c18517deull);
    return false;
}
struct SeedEntry { uint32_t vs, fs, lb; };
constexpr SeedEntry kBeforeSeed[] = { // (vgx_backend_gl.cpp kPlySeed until seedrun31)
    {0x6891d205u, 0xf30e1197u, 0x12c00u}, {0x464daa45u, 0x57115485u, 0x16400u}, {0x65a4c4beu, 0x0f0752cfu, 0x16400u},
    {0x92b0a9fcu, 0x54cc606cu, 0x16400u}, {0x65a4c4beu, 0xbb0428c1u, 0x16400u}, {0x65a4c4beu, 0x4aaa1954u, 0x16400u},
    {0xe801e2e8u, 0x94813dc2u, 0x16400u}, {0x07d4a19fu, 0x4f48e5e0u, 0x16400u}, {0x89624582u, 0x2aa6c7a6u, 0x16451u},
    {0x277812b5u, 0xb24ec6e8u, 0x16c00u}, {0x7303aa2cu, 0x0f0660bdu, 0x16c00u},
};
constexpr uint32_t kPly = 0x504c5952u, kMenu = 0x4d454e55u, kStamp = 0x53544147u;
static std::vector<Rec> Load(const char* path) {
    std::ifstream in(path, std::ios::binary);
    char magic[8]; in.read(magic, 8);
    assert(in && std::string(magic, 8) == "VGXSTW01");
    std::vector<Rec> out;
    Rec r;
    while (in.read(reinterpret_cast<char*>(&r), sizeof(r))) out.push_back(r);
    assert(in.eof() && !in.gcount());
    return out;
}
struct Entry { uint16_t vs, fs; uint32_t lb; bool operator==(const Entry& o) const { return vs == o.vs && fs == o.fs && lb == o.lb; } };
struct Ids {
    std::map<uint64_t, uint16_t> vs, fs;
    std::map<uint32_t, std::vector<uint16_t>> vs32, fs32;
    void Add(const std::vector<Rec>& all) {
        for (const Rec& r : all) {
            if (r.stage == kStamp) continue;
            if (vs.emplace(r.vs, static_cast<uint16_t>(vs.size())).second) vs32[static_cast<uint32_t>(r.vs)].push_back(vs.at(r.vs));
            if (fs.emplace(r.fs, static_cast<uint16_t>(fs.size())).second) fs32[static_cast<uint32_t>(r.fs)].push_back(fs.at(r.fs));
        }
        assert(vs.size() < missing && fs.size() < missing);
    }
};
static bool Linked(uint16_t vs, uint16_t fs) { return ((uint32_t(vs) << 16 | fs) * 2654435761u) % 7 == 0; }
// before = true: the rule until seedrun31; else the data rule for scenario `cur`. Returns the queue (own + player), the head rearranged.
static std::deque<Entry> Build(const std::vector<Rec>& all, uint32_t stage, const Ids& ids, bool before, unsigned cur, size_t* firstN, unsigned* seedQueued) {
    std::vector<Rec> own, ply;
    for (const Rec& r : all) {
        if (r.stage == stage) own.push_back(r);
        else if (r.stage == kPly) ply.push_back(r);
    }
    Queue q;
    std::deque<Entry> out;
    std::map<uint32_t, uint32_t> firstIds;
    std::set<uint32_t> open;
    std::vector<uint32_t> order;
    size_t curRecs = own.size();
    if (before) for (uint32_t i = 0; i < own.size(); ++i) order.push_back(i);
    else curRecs = ScenarioFirst(own, cur, order);
    size_t ownCurN = ~size_t{0};
    for (size_t oi = 0; oi < order.size(); ++oi) {
        if (oi == curRecs) ownCurN = out.size();
        const Rec& r = own[order[oi]];
        const uint16_t v = ids.vs.at(r.vs), f = ids.fs.at(r.fs);
        const bool prio = before ? ((r.lb & slowTag) || BeforeHistorical(stage, r.vs, r.fs, r.lb)) : Priority(r.lb, cur);
        if (prio) firstIds.emplace(uint32_t(v) << 16 | f, r.lb & drawMask);
        if (!q.First(v, f, r.lb)) continue;
        out.push_back({v, f, r.lb & drawMask});
    }
    const size_t ownN = out.size();
    if (ownCurN == ~size_t{0}) ownCurN = ownN;
    size_t plyN = 0;
    for (const Rec& r : ply) {
        const uint16_t v = ids.vs.at(r.vs), f = ids.fs.at(r.fs);
        if (!q.First(v, f, r.lb)) continue;
        out.push_back({v, f, r.lb & drawMask});
        ++plyN;
    }
    *seedQueued = 0;
    if (before) // (the built-in seed, after every player record: low 32 bits of the registered key hashes; a half that is not registered: nothing)
        for (const SeedEntry& sd : kBeforeSeed) {
            const auto v = ids.vs32.find(sd.vs), f = ids.fs32.find(sd.fs);
            if (v == ids.vs32.end() || f == ids.fs32.end()) continue;
            assert(v->second.size() == 1 && f->second.size() == 1); // (two keys ending alike: the run-time pick was an accident of hash-map order)
            if (!q.First(v->second[0], f->second[0], sd.lb)) continue;
            out.push_back({v->second[0], f->second[0], sd.lb});
            ++plyN;
            ++*seedQueued;
        }
    if (!before) OthersBehindPlayer(out, ownCurN, ownN - ownCurN, plyN);
    const size_t seg = ownN + plyN;
    const auto unlinked = [](const Entry& e) { return !Linked(e.vs, e.fs); };
    for (size_t i = 0; i < seg; ++i) if (unlinked(out[i])) open.insert(uint32_t(out[i].vs) << 16 | out[i].fs);
    const auto prio = [&](const Entry& e) {
        const uint32_t k = uint32_t(e.vs) << 16 | e.fs;
        const auto it = firstIds.find(k);
        return it != firstIds.end() && it->second == e.lb && open.count(k);
    };
    if (before) { // (the code until seedrun31: two stable partitions over the head)
        if (!open.empty()) std::stable_partition(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(seg), unlinked);
        *firstN = firstIds.empty() ? 0 : static_cast<size_t>(std::stable_partition(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(seg), prio) - out.begin());
    } else {
        *firstN = ArrangeHead(out, seg, !open.empty(), !firstIds.empty(), unlinked, prio);
    }
    return out;
}
static int Replay(const char* original, const char* tagged) {
    const std::vector<Rec> a = Load(original), b = Load(tagged);
    Ids ids;
    ids.Add(a);
    ids.Add(b);
    // the card of a console that played the earlier package: the old list, then the tagged package's records it does not hold (StageAdd's exact duplicate check)
    std::vector<Rec> card = a;
    {
        std::set<std::tuple<uint32_t, uint64_t, uint64_t, uint32_t>> have;
        for (const Rec& r : a) have.emplace(r.stage, r.vs, r.fs, r.lb);
        for (const Rec& r : b) if (r.stage != kStamp && have.emplace(r.stage, r.vs, r.fs, r.lb).second) card.push_back(r);
    }
    std::set<uint32_t> stages;
    for (const Rec& r : a) if (r.stage != kPly && r.stage != kMenu && r.stage != kStamp) stages.insert(r.stage);
    unsigned withNine = 0, seedTotal = 0;
    size_t entries = 0;
    for (const uint32_t stage : stages) {
        size_t f0 = 0, f1 = 0, f2 = 0, f3 = 0, f4 = 0;
        unsigned s0 = 0, s1 = 0;
        const std::deque<Entry> before = Build(a, stage, ids, true, 0, &f0, &s0);
        const std::deque<Entry> data = Build(b, stage, ids, false, 0, &f1, &s1);
        const std::deque<Entry> data3 = Build(b, stage, ids, false, 3, &f2, &s1);
        const std::deque<Entry> merged = Build(card, stage, ids, false, 0, &f4, &s1);
        if (!(before == data && before == data3 && before == merged && f0 == f1 && f1 == f2 && f1 == f4)) {
            size_t i = 0;
            while (i < before.size() && i < data.size() && before[i] == data[i]) ++i;
            std::printf("replay stage %08x: FAIL: queues differ (before %zu entries, data %zu, scenario 3 %zu, card+package %zu; first difference at %zu; head %zu / %zu)\n",
                        stage, before.size(), data.size(), data3.size(), merged.size(), i, f0, f1);
            return 1;
        }
        entries += before.size();
        seedTotal = s0;
        unsigned nine = 0;
        for (const Rec& r : a) if (r.stage == stage && BeforeHistorical(stage, r.vs, r.fs, r.lb)) ++nine;
        if (!nine) continue;
        ++withNine;
        const std::deque<Entry> untagged = Build(a, stage, ids, false, 0, &f3, &s1);
        std::vector<size_t> places;
        for (const Rec& r : a) {
            if (r.stage != stage || !BeforeHistorical(stage, r.vs, r.fs, r.lb)) continue;
            const Entry e{ids.vs.at(r.vs), ids.fs.at(r.fs), r.lb & drawMask};
            for (size_t i = 0; i < before.size(); ++i) if (before[i] == e) { places.push_back(i); break; }
        }
        assert(f3 == 0 || f3 < f0); // (the untagged list: the historical ones are not in front; the tags are what does it)
        std::string at;
        for (size_t p : places) at += (at.empty() ? "" : ",") + std::to_string(p);
        std::printf("replay stage %08x: %zu queue entries, %zu priority entries at the head; the %u historical records at %s (untagged list without the old rule: %zu at the head)\n",
                    stage, before.size(), f0, nine, at.c_str(), f3);
    }
    std::printf("replay: %zu stages, %zu queue entries: identical queues, before (built-in hashes and seed) and from the tagged list alone (scenario not known, 3, card+package); "
                "%u stages with historical records; the built-in seed queued %u variants a stage, now PLYR records PASS\n", stages.size(), entries, withNine, seedTotal);
    return 0;
}
int main(int argc, char** argv) {
    Cases();
    if (argc == 3) return Replay(argv[1], argv[2]);
    return 0;
}
