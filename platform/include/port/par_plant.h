// GXV_PAR_PLANT (revisit23 round 2, 8 Nov 2026): who may read a plant's LIVE state during the drawn turn's flush (header-only; used by
// decomp/src/Game/LiveActor/ModelManager.cpp and LiveActor.cpp, host proof tests/par_plant.cpp under ThreadSanitizer / ASan).
//
// GXV_PAR_FLUSH (par2) made every flush job on the game thread (the key and the walk's inputs copied from the model) and gave only the
// walk to the worker; r87's console run: 0.8 ms a picture of making, nothing saved. With GXV_PAR_PLANT the WORKER makes the jobs of the
// flower groups' plants itself (PlantMember: GXV_ANIM_LOD_FREE defers every one of them to the flush) from the model's live state, and
// the game thread only lists them. That is safe only while the game thread does not write what the worker reads (the actor's latch,
// the XanimePlayer, the core, the base matrix): each job has a phase byte, and
//     worker:      kFree -> kBuilding (CAS; it may read the live state) ... job inputs and key written ... -> kBuilt (release store)
//     game thread: kFree -> kGame (CAS, before the flush touches the actor): the worker never reads that model (it skips the job);
//                  kBuilding: wait until kBuilt (the worker copies, a few microseconds); kBuilt: go on (the worker reads only the job
//                  from then on, and the job is used only on par-anim's word-for-word key rule at the model's real computation).
// The two threads meet once a flush (the worker from the end of the list, the game thread from the start), so a wait is rare; it is
// counted and timed, and a wait of more than PlantMaxWaitUs pauses the mode (ModelManager.cpp).
#pragma once
#include "port/par_exec.h"  // (NowUs, CpuRelax)
#include "port/wait_bound.h"  // (hitch30: BoundedWait)
#include <atomic>
#include <cstdint>
namespace port {
namespace par {
enum : uint8_t { kPlFree = 0, kPlBuilding = 1, kPlBuilt = 2, kPlGame = 3 };
// The worker, before it reads the model's live state: true = it owns the read (build), false = the game thread got there first.
inline bool PlantWorkerBegin(std::atomic< uint8_t >& phase) {
    uint8_t v = kPlFree;
    return phase.compare_exchange_strong(v, kPlBuilding, std::memory_order_acq_rel);
}
// The worker, once everything it read is in the job (inputs, key; ok or not): the game thread may write the live state again.
inline void PlantWorkerBuilt(std::atomic< uint8_t >& phase) {
    phase.store(kPlBuilt, std::memory_order_release);
}
// The game thread, before it writes anything of the model: kPlGame (the worker never built it) or kPlBuilt (built; the job's fields
// are visible). *waitedUs: the time spent waiting for a build in progress (0 almost always).
inline uint8_t PlantGameOwn(std::atomic< uint8_t >& phase, uint64_t* waitedUs) {
    *waitedUs = 0;
    uint8_t v = kPlFree;
    if (phase.compare_exchange_strong(v, kPlGame, std::memory_order_acq_rel)) {
        return kPlGame;
    }
    if (v == kPlBuilding) {
        // (hitch30: the worker is in the middle of copying; it may be a priority step below this thread, on this thread's core: spin
        // at most GXV_BOUND_SPIN_US, then nap so that it can run)
        *waitedUs = BoundedWait(
                        [&]() {
                            v = phase.load(std::memory_order_acquire);
                            return v == kPlBuilding;
                        },
                        PlantWaitCensus(), WaitBoundSpinUs(), WaitBoundNapUs()) + 1;
    }
    return v;  // (kPlBuilt; kPlGame only if this was called twice)
}
// GXV_PAR_PLANT_EARLY (parplant23, 8 Nov): the batch is listed at the END of the turn before the drawn one (its calc-anim is over; no
// update() runs before the flush), so the worker has the retrace wait and the next turn's start to make and walk the jobs. Listed at the
// drawn turn's start (2.3 round 2), the worker never got there first on the console: the deferred draw-done wait (GXV_DRAWDONE_DEFER=2)
// is taken at calc-anim, so the drawn turn goes from the listing straight into the flush, whose first entries are the plants (they
// defer in CalcAnimType_MapObj, the second calc-anim list). Whether the next turn draws is told only at its start (frame skipping), so it
// is predicted here from the last two turns: after a skipped turn, or after two drawn ones (60 pictures), the next one draws; after a
// drawn turn that followed a skipped one (30 pictures: D S D S), it does not. A wrong "draws" costs only the worker's time (the batch
// is ended, waiting for the one job the worker may be in, before the next update(): port_par_plant_before_update); a wrong "does not"
// lists the batch at the drawn turn's start as before.
//   mode 0: never early (the 2.3 round 2 place only); 1: by the prediction; 2: at the end of every turn.
struct PlantTurns {
    uint8_t last = 1, prev = 1;  // (1 = drawn) the last two turns
    void Note(bool drawn) {
        prev = last;
        last = drawn ? 1 : 0;
    }
    bool NextDraws(int mode) const {
        if (mode <= 0) {
            return false;
        }
        if (mode >= 2) {
            return true;
        }
        return !(last == 1 && prev == 0);
    }
};
}  // namespace par
}  // namespace port
