// smooth25 (GXV_STAGEBUILD_CACHE): the stage queue's "programs other stages' lists share" tally (vgx_backend_gl.cpp StageBuild), as a header so that the host test
// (tests/stage_tally_model.cpp) runs the same code against the plain loop.
//
// The plain loop visits every record of every stage but the target and the player list, four hash operations each (7.7k records in 41 stages on the console: ~78 ms
// on the render thread at every stage change, c54 10:48:48 and 10:48:53, the same for two different stages). The cached form does that once per state of the sets
// (record count, stage count, shader registrations) for all stages but the player list, remembering for each program key the count, the first stage's layout word
// and the second stage's; for a target it takes the target's own count off the keys the target holds (one pass over the target's records) and, when the first
// stage in the map's order IS the target, the second stage's layout word. That is the plain loop's result (counts, layout word of the first record of the first
// stage that is not the target, in the map's order) without the loop; the test compares the two on random sets, including the order of the sorted result.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vgx::stagetally {

struct Cnt {
    unsigned n = 0;
    uint32_t lb = 0;
    uint16_t vs = 0, fs = 0;
};
using CntMap = std::unordered_map<uint32_t, Cnt>;
using Order = std::vector<std::pair<uint64_t, uint32_t>>;  // (0xFFFF - count) << 32 | key, key: the program key (vs id << 16 | fs id)

// Sets: std::unordered_map<uint32_t, S>, S::recs a vector of records with vs, fs (key hashes) and lb. Ids: unordered_map<uint64_t, uint16_t> from key hash to shader id.
template <class Sets, class Ids>
void PlainTally(const Sets& sets, uint32_t target, uint32_t playerKey, const Ids& vsIds, const Ids& fsIds, CntMap& out) {
    std::unordered_set<uint32_t> mine;
    for (const auto& kv : sets) {
        if (kv.first == target || kv.first == playerKey) continue;
        mine.clear();
        for (const auto& r : kv.second.recs) {
            const auto v = vsIds.find(r.vs), f = fsIds.find(r.fs);
            if (v == vsIds.end() || f == fsIds.end()) continue;
            const uint32_t k = (static_cast<uint32_t>(v->second) << 16) | f->second;
            if (!mine.insert(k).second) continue;
            Cnt& c = out[k];
            if (!c.n) { c.lb = r.lb; c.vs = v->second; c.fs = f->second; }
            ++c.n;
        }
    }
}

// The keys with at least minN stages that `skip` does not hold, most shared first (ties by key: the keys are unique, so the order is total).
template <class Skip>
void PlainOrder(const CntMap& in, unsigned minN, const Skip& skip, Order& out) {
    for (const auto& kv : in)
        if (kv.second.n >= minN && !skip(kv.first)) out.emplace_back((uint64_t{0xFFFFu - std::min(kv.second.n, 0xFFFFu)} << 32) | kv.first, kv.first);
    std::sort(out.begin(), out.end());
}

class Cache {
public:
    // The sets' state the tally was made for: when it changes the tally is made again.
    template <class Sets, class Ids, class Skip>
    void Select(const Sets& sets, size_t recs, unsigned regs, uint32_t target, uint32_t playerKey, const Ids& vsIds, const Ids& fsIds, unsigned minN, const Skip& skip,
                CntMap& cnt, Order& order, bool* rebuilt = nullptr) {
        MakeWhole(sets, recs, regs, playerKey, vsIds, fsIds, rebuilt);
        if (candMin_ != minN) RebuildCand(minN);  // (hitch25: only the keys with at least minN stages can pass the filter below: n - 1 >= minN or n >= minN)
        std::unordered_set<uint32_t> mine;
        const auto ti = sets.find(target);
        if (ti != sets.end())
            for (const auto& r : ti->second.recs) {
                const auto v = vsIds.find(r.vs), f = fsIds.find(r.fs);
                if (v == vsIds.end() || f == fsIds.end()) continue;
                mine.insert((static_cast<uint32_t>(v->second) << 16) | f->second);
            }
        for (const auto& kc : cand_) {
            const uint32_t key = kc.first;
            const Entry& e = *kc.second;
            const bool inTarget = mine.count(key) != 0;
            const unsigned n = e.n - (inTarget ? 1u : 0u);
            if (n < minN || skip(key)) continue;
            Cnt& c = cnt[key];
            c.n = n;
            c.vs = e.vs;
            c.fs = e.fs;
            c.lb = (e.first == target && inTarget) ? e.lb2 : e.lb1;
            order.emplace_back((uint64_t{0xFFFFu - std::min(n, 0xFFFFu)} << 32) | key, key);
        }
        std::sort(order.begin(), order.end());
    }
    // openstall26 (GXV_STAGEBUILD_FAST): the same result as Select for the first maxOut entries of `order`, without visiting the other candidates. Select walks every key at least
    // minN stages hold (c80: 75-80 ms of the 105-ms stage queue build on the render thread, with the tally fresh), makes a Cnt for each and sorts them, then the caller takes the
    // first StageCommonMax (160). Here the candidates are sorted once per tally (PrepareOrder, from the background slices or the first Select after a refresh) and a target only
    // moves the keys it holds down by one stage: those are taken apart, sorted by their lowered count, and merged with the rest of the sorted list until maxOut entries pass `skip`.
    template <class Sets, class Ids, class Skip>
    void SelectTop(const Sets& sets, size_t recs, unsigned regs, uint32_t target, uint32_t playerKey, const Ids& vsIds, const Ids& fsIds, unsigned minN, const Skip& skip,
                   size_t maxOut, CntMap& cnt, Order& order, bool* rebuilt = nullptr) {
        MakeWhole(sets, recs, regs, playerKey, vsIds, fsIds, rebuilt);
        PrepareOrder(minN);
        std::unordered_set<uint32_t> mine;
        const auto ti = sets.find(target);
        if (ti != sets.end())
            for (const auto& r : ti->second.recs) {
                const auto v = vsIds.find(r.vs), f = fsIds.find(r.fs);
                if (v == vsIds.end() || f == fsIds.end()) continue;
                mine.insert((static_cast<uint32_t>(v->second) << 16) | f->second);
            }
        Order adj;  // the target's own keys, with the count one lower
        for (const uint32_t key : mine) {
            const auto t = tally_.find(key);
            if (t == tally_.end() || t->second.n < 1 + minN) continue;
            const unsigned n = t->second.n - 1;
            adj.emplace_back((uint64_t{0xFFFFu - std::min(n, 0xFFFFu)} << 32) | key, key);
        }
        std::sort(adj.begin(), adj.end());
        size_t i = 0, j = 0;
        while (order.size() < maxOut) {
            while (i < sorted_.size() && mine.count(sorted_[i].second)) ++i;  // (taken apart above)
            const bool haveA = i < sorted_.size(), haveB = j < adj.size();
            if (!haveA && !haveB) break;
            const bool fromB = haveB && (!haveA || adj[j] < sorted_[i]);
            const std::pair<uint64_t, uint32_t> it = fromB ? adj[j++] : sorted_[i++];
            const uint32_t key = it.second;
            if (skip(key)) continue;
            const Entry& e = tally_.find(key)->second;
            Cnt& c = cnt[key];
            c.n = 0xFFFFu - static_cast<unsigned>(it.first >> 32);
            c.vs = e.vs;
            c.fs = e.fs;
            c.lb = (e.first == target && fromB) ? e.lb2 : e.lb1;
            order.push_back(it);
        }
    }
    // The candidates in the order Select sorts them (count down, key up), for this minN. Cheap to call again; the first call after a tally is made does the sort.
    void PrepareOrder(unsigned minN) {
        if (sortedMin_ == minN) return;
        sorted_.clear();
        for (const auto& kv : tally_)
            if (kv.second.n >= minN) sorted_.emplace_back((uint64_t{0xFFFFu - std::min(kv.second.n, 0xFFFFu)} << 32) | kv.first, kv.first);
        std::sort(sorted_.begin(), sorted_.end());
        sortedMin_ = minN;
    }
    bool OrderReady(unsigned minN) const { return sortedMin_ == minN; }
    size_t Keys() const { return tally_.size(); }

    // ---- hitch25 (GXV_STAGEBUILD_SLICE) the tally made again in slices, ahead of the stage change that needs it -----------------------------------------
    // Select() makes the tally whole when the sets' state (record count, stage count, shader registrations) changed since it was made: ~80-100 ms on the
    // render thread, at the stage change, where the star select opens (c76: 21 of 22 stage changes, "periodic job" 113-125 ms). The records are added during
    // play, so the tally is stale at every stage change. BeginRefresh / StepRefresh make the same tally as Select would, a time budget at a time, from the
    // frames after the state has stopped changing; Select then finds it fresh and only does the per-target pass.
    bool Fresh(size_t recs, size_t stages, unsigned regs) const { return !building_ && recs == keyRecs_ && stages == keyStages_ && regs == keyRegs_; }
    bool Building() const { return building_; }
    template <class Sets>
    void BeginRefresh(const Sets& sets, size_t recs, unsigned regs, uint32_t playerKey) {
        work_.clear();
        pendKeys_.clear();
        for (const auto& kv : sets) if (kv.first != playerKey) pendKeys_.push_back(kv.first);  // (the sets' own order: the tally's "first stage" follows it)
        pos_ = 0;
        recPos_ = 0;
        mine_.clear();
        pendRecs_ = recs;
        pendStages_ = sets.size();
        pendRegs_ = regs;
        building_ = true;
    }
    // Works for about budgetMs (the clock is read every 64 records); true when the tally is complete and in place.
    template <class Sets, class Ids, class Clock>
    bool StepRefresh(const Sets& sets, const Ids& vsIds, const Ids& fsIds, double budgetMs, Clock&& nowMs) {
        if (!building_) return true;
        const double t0 = nowMs();
        unsigned sinceClock = 0;
        while (pos_ < pendKeys_.size()) {
            const uint32_t stageKey = pendKeys_[pos_];
            const auto si = sets.find(stageKey);
            if (si == sets.end()) { ++pos_; recPos_ = 0; mine_.clear(); continue; }
            const auto& recs = si->second.recs;
            while (recPos_ < recs.size()) {
                const auto& r = recs[recPos_++];
                const auto v = vsIds.find(r.vs), f = fsIds.find(r.fs);
                if (v != vsIds.end() && f != fsIds.end()) {
                    const uint32_t k = (static_cast<uint32_t>(v->second) << 16) | f->second;
                    if (mine_.insert(k).second) {
                        Entry& t = work_[k];
                        if (!t.n) { t.first = stageKey; t.lb1 = static_cast<uint32_t>(r.lb); t.vs = v->second; t.fs = f->second; }
                        else if (!t.has2) { t.lb2 = static_cast<uint32_t>(r.lb); t.has2 = true; }
                        ++t.n;
                    }
                }
                if ((++sinceClock & 63u) == 0 && nowMs() - t0 >= budgetMs) return false;
            }
            ++pos_;
            recPos_ = 0;
            mine_.clear();
            if (pos_ < pendKeys_.size() && nowMs() - t0 >= budgetMs) return false;
        }
        tally_.swap(work_);
        work_.clear();
        candMin_ = ~0u;
        sortedMin_ = ~0u;
        keyRecs_ = pendRecs_;
        keyStages_ = pendStages_;
        keyRegs_ = pendRegs_;
        building_ = false;
        return true;
    }

private:
    template <class Sets, class Ids>
    void MakeWhole(const Sets& sets, size_t recs, unsigned regs, uint32_t playerKey, const Ids& vsIds, const Ids& fsIds, bool* rebuilt) {
        const bool stale = recs != keyRecs_ || sets.size() != keyStages_ || regs != keyRegs_;
        if (rebuilt) *rebuilt = stale;
        if (stale) {
            building_ = false;  // (hitch25: a sliced refresh in progress is left; this is the whole tally, made now)
            work_.clear();
            candMin_ = ~0u;
            sortedMin_ = ~0u;
            tally_.clear();
            std::unordered_set<uint32_t> mine;
            for (const auto& kv : sets) {
                if (kv.first == playerKey) continue;
                mine.clear();
                for (const auto& r : kv.second.recs) {
                    const auto v = vsIds.find(r.vs), f = fsIds.find(r.fs);
                    if (v == vsIds.end() || f == fsIds.end()) continue;
                    const uint32_t k = (static_cast<uint32_t>(v->second) << 16) | f->second;
                    if (!mine.insert(k).second) continue;
                    Entry& t = tally_[k];
                    if (!t.n) { t.first = kv.first; t.lb1 = static_cast<uint32_t>(r.lb); t.vs = v->second; t.fs = f->second; }
                    else if (!t.has2) { t.lb2 = static_cast<uint32_t>(r.lb); t.has2 = true; }
                    ++t.n;
                }
            }
            keyRecs_ = recs;
            keyStages_ = sets.size();
            keyRegs_ = regs;
        }
    }
    struct Entry {
        unsigned n = 0;
        uint32_t lb1 = 0, lb2 = 0, first = 0;
        uint16_t vs = 0, fs = 0;
        bool has2 = false;
    };
    void RebuildCand(unsigned minN) {
        cand_.clear();
        for (const auto& kv : tally_) if (kv.second.n >= minN) cand_.emplace_back(kv.first, &kv.second);
        candMin_ = minN;
    }
    std::unordered_map<uint32_t, Entry> tally_;
    size_t keyRecs_ = ~size_t{0}, keyStages_ = ~size_t{0};
    unsigned keyRegs_ = ~0u;
    // (hitch25) the keys at least minN stages hold (the only ones the per-target pass can pass), the refresh in progress
    std::vector<std::pair<uint32_t, const Entry*>> cand_;
    unsigned candMin_ = ~0u;
    std::vector<std::pair<uint64_t, uint32_t>> sorted_;  // (openstall26) the candidates sorted, for sortedMin_
    unsigned sortedMin_ = ~0u;
    std::unordered_map<uint32_t, Entry> work_;
    std::vector<uint32_t> pendKeys_;
    std::unordered_set<uint32_t> mine_;
    size_t pos_ = 0, recPos_ = 0, pendRecs_ = 0, pendStages_ = 0;
    unsigned pendRegs_ = 0;
    bool building_ = false;
};

}  // namespace vgx::stagetally
