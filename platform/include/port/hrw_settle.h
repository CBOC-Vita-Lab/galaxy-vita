#pragma once
// stutter37 (15 Nov 2026): GXV_HRW_SETTLE and GXV_HRW_SETTLE_RESUME: when the pairs of the re-projected 60 (GXV_HRW=5) may begin. Pure logic: tests/hrw_settle_test.cpp runs this code.
//
// hrw29's rule (vgx_galaxy.cpp HrwSettled): no pair until GXV_HRW_SETTLE DRAWN turns have been played with the stage live (a real stage, the screen open, no scene being
// loaded); any turn that is not live (a pause menu, a closed fade, a load, a menu) starts the count again. Before the pairs begin only every second turn is drawn, so the
// Performance mode's 200 is 6.7 s of 30 pictures a second, and it is paid again after every pause and every closed fade inside the stage (console 15 Nov, the 4-minute
// Honeyhive run 3090c: 163 s of 280 s in the stage without pairs; the stress route: 119 s of about 390 s).
//
// GXV_HRW_SETTLE_RESUME=<drawn turns> (default -1 = as GXV_HRW_SETTLE: nothing changes): the count that applies when the SAME scene had already settled and nothing was
// loaded since: the screen was only covered (a pause, a closed fade). A load, another scene or the title starts the whole count as before. Never more than GXV_HRW_SETTLE.
#include <cstdint>

namespace port::hrwsettle {

class Settle {
public:
    // need: GXV_HRW_SETTLE. resume: GXV_HRW_SETTLE_RESUME (negative: as need).
    void Configure(unsigned need, long resume) {
        need_ = need;
        resume_ = resume < 0 || static_cast<unsigned long>(resume) > need ? need : static_cast<unsigned>(resume);
    }
    // One call per drawn turn (advance true) and per undrawn turn of a pair (advance false: it only tests).
    // key: the scene's key (0: none). live: in play (a real stage, the screen open, no menu, no load). loading: a scene is being loaded.
    // True: pairs may be made.
    bool Update(uint32_t key, bool live, bool loading, bool advance) {
        if (loading || key == 0 || (live && key != doneKey_)) doneKey_ = 0;   // (what had settled is gone: a load, no scene, another scene)
        if (!live || key != setKey_) {
            setKey_ = live ? key : 0;
            count_ = 0;
            resumed_ = live && doneKey_ == key && key != 0;
            return false;
        }
        const unsigned need = resumed_ ? resume_ : need_;
        if (advance && count_ < need) ++count_;
        if (count_ < need) return false;
        if (doneKey_ != key) { doneKey_ = key; }
        return true;
    }
    bool Resumed() const { return resumed_; }       // the count in progress (or met) is the short one
    unsigned Count() const { return count_; }
    unsigned Need() const { return resumed_ ? resume_ : need_; }

private:
    unsigned need_ = 90, resume_ = 90, count_ = 0;
    uint32_t setKey_ = 0;    // the scene the count is for (0: not live)
    uint32_t doneKey_ = 0;   // the scene that has settled once and was not loaded since (0: none)
    bool resumed_ = false;
};

}  // namespace port::hrwsettle
