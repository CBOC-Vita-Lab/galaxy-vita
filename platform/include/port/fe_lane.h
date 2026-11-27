#pragma once
// GXV_FE_LANE (pace33 PATCH4, 14 Nov 2026): where a picture's time goes between its turn's start and its hand-over to the render thread, and which part made the late ones late.
// Pure logic (no clock, no globals): tests/pace33_test.cpp runs this code.
//
// WHY. With the standing display queue drained (GXV_DRAIN33) a picture that reaches the render thread 8-14 ms later than usual misses its refresh. The console logs (batches 4 and 5) say
// THAT pictures arrive late ("hitch: ... no picture arrived") but not where the time went: only 5-second means of each thread's CPU. This census times every first picture on the player
// thread (five clock reads and one thread-CPU read a picture) and prints one line per 5 s.
//
// The parts of one picture (ms), all on one clock:
//   game    turn start -> the game's draw of the picture ends (its flushes and draw; PATCH5: the game thread notes that moment itself)
//   tail    the game's draw has ended and the player is still parsing, up to the display copy: how far the front end runs behind the game
//   join    the display copy waits for the finishing thread's records
//   pass    the front end's own end-of-picture work (the present pass), without the queue wait
//   queue   the hand-over waits for room in the render thread's queue
//   lost    wall time the player was neither asleep nor on the CPU (preempted, or blocked inside join / queue)
// A picture is LATE when its hand-over is `lateUs` (4 ms) or more after the window's median hand-over. Each late picture is booked to the part that is furthest over its own median.
#include <algorithm>
#include <cstdint>
#include <stdio.h>

namespace port::felane {

enum Part : int { kGame = 0, kTail = 1, kJoin = 2, kPass = 3, kQueue = 4, kParts = 5 };
struct Pic {
    uint32_t us[kParts] = {};   // the five parts
    uint32_t lostUs = 0;        // not asleep, not on the CPU
    uint32_t turnGapUs = 0;     // this drawn turn's start minus the drawn turn's before it (33.37 ms when the game is on time)
    uint32_t Total() const { return us[0] + us[1] + us[2] + us[3] + us[4]; }
};

class Census {
public:
    static constexpr unsigned kMax = 320;
    void Add(const Pic& p) { if (n_ < kMax) pics_[n_++] = p; else ++dropped_; }
    unsigned Count() const { return n_; }
    void Clear() { n_ = 0; dropped_ = 0; }
    struct Out {
        unsigned n = 0, late = 0, lateBy[kParts] = {}, lateTurn = 0;
        uint32_t med[kParts] = {}, p95[kParts] = {}, max[kParts] = {};
        uint32_t totMed = 0, totP95 = 0, totMax = 0, lostMean = 0, lostMax = 0, gapP95Dev = 0;
        uint32_t worst[kParts] = {}, worstLost = 0;   // the latest picture's parts
    };
    // lateUs: see above.
    Out Summarise(uint32_t lateUs = 4000) const {
        Out o;
        o.n = n_;
        if (!n_) return o;
        static uint32_t v[kMax];   // (one caller: the player thread)
        auto pct = [&](unsigned k, unsigned num, unsigned den) { std::nth_element(v, v + k, v + n_); (void)num; (void)den; return v[k]; };
        const unsigned iMed = n_ / 2, i95 = (n_ * 95) / 100 < n_ ? (n_ * 95) / 100 : n_ - 1;
        for (int p = 0; p < kParts; ++p) {
            for (unsigned i = 0; i < n_; ++i) { v[i] = pics_[i].us[p]; o.max[p] = std::max(o.max[p], v[i]); }
            o.med[p] = pct(iMed, 1, 2);
            o.p95[p] = pct(i95, 95, 100);
        }
        uint64_t lost = 0;
        unsigned worstI = 0;
        for (unsigned i = 0; i < n_; ++i) {
            v[i] = pics_[i].Total();
            if (v[i] > o.totMax) { o.totMax = v[i]; worstI = i; }
            lost += pics_[i].lostUs; o.lostMax = std::max(o.lostMax, pics_[i].lostUs);
        }
        o.totMed = pct(iMed, 1, 2);
        o.totP95 = pct(i95, 95, 100);
        o.lostMean = static_cast<uint32_t>(lost / n_);
        for (unsigned i = 0; i < n_; ++i) { const uint32_t g = pics_[i].turnGapUs; v[i] = g > 33367u ? g - 33367u : 33367u - g; }
        o.gapP95Dev = pct(i95, 95, 100);
        for (int p = 0; p < kParts; ++p) o.worst[p] = pics_[worstI].us[p];
        o.worstLost = pics_[worstI].lostUs;
        for (unsigned i = 0; i < n_; ++i) {
            const Pic& q = pics_[i];
            if (q.Total() < o.totMed + lateUs) continue;
            ++o.late;
            int best = 0;
            int64_t bestOver = -1;
            for (int p = 0; p < kParts; ++p) {
                const int64_t over = static_cast<int64_t>(q.us[p]) - static_cast<int64_t>(o.med[p]);
                if (over > bestOver) { bestOver = over; best = p; }
            }
            ++o.lateBy[best];
            if (q.turnGapUs > 33367u + lateUs / 2) ++o.lateTurn;
        }
        return o;
    }

private:
    Pic pics_[kMax];
    unsigned n_ = 0, dropped_ = 0;
};

inline int Format(char* out, unsigned cap, const Census::Out& o, double seconds) {
    auto ms = [](uint32_t us) { return us / 1000.0; };
    return snprintf(out, cap, "fe-lane: %.1f s, %u first pictures; turn start to hand-over %.1f ms median, %.1f at the 95th, %.1f longest; parts median / 95th / longest: game (turn start to the last "
                              "bytes written) %.1f / %.1f / %.1f, front end behind the game's draw end %.1f / %.1f / %.1f, join with the finishing thread %.1f / %.1f / %.1f, present pass "
                              "%.1f / %.1f / %.1f, wait for the render queue %.1f / %.1f / %.1f; LATE (4 ms or more over the median) %u: by game %u, front end behind %u, join %u, present pass %u, "
                              "queue %u (of them %u in a turn that itself started 2 ms or more late); player neither asleep nor on the CPU %.2f ms a picture, %.1f at most; drawn-turn start interval "
                              "off 33.4 ms by %.1f at the 95th; the latest picture: game %.1f, front end behind %.1f, join %.1f, pass %.1f, queue %.1f, off the CPU %.1f",
                    seconds, o.n, ms(o.totMed), ms(o.totP95), ms(o.totMax), ms(o.med[0]), ms(o.p95[0]), ms(o.max[0]), ms(o.med[1]), ms(o.p95[1]), ms(o.max[1]), ms(o.med[2]), ms(o.p95[2]), ms(o.max[2]),
                    ms(o.med[3]), ms(o.p95[3]), ms(o.max[3]), ms(o.med[4]), ms(o.p95[4]), ms(o.max[4]), o.late, o.lateBy[0], o.lateBy[1], o.lateBy[2], o.lateBy[3], o.lateBy[4], o.lateTurn,
                    ms(o.lostMean), ms(o.lostMax), ms(o.gapP95Dev), ms(o.worst[0]), ms(o.worst[1]), ms(o.worst[2]), ms(o.worst[3]), ms(o.worst[4]), ms(o.worstLost));
}

}  // namespace port::felane
