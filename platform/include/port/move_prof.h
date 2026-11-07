// GXV_ANIM_PROF=1/2/3 (hivelogic, 6 Nov): the update-scene timers (platform/src/port/perf.cpp, "move-prof:" in the basic log).
//  - Every category list (CategoryList::execute) is timed whole through port_anim_lod_set_category: two clock reads a category,
//    about fifty a turn. On with GXV_ANIM_PROF=1, 2 or 3, in every build (the lines go to log.txt through port_log_summary).
//  - PORT_MOVE_SYS(id): one per-turn system, timed inclusive (its outermost call only), with GXV_ANIM_PROF=1 or 3:
//      0 gravity (PlanetGravityManager::calcTotalGravityVector)   1 area lookups (AreaObjMgr::find_in)
//    Off: one load of a global and a branch not taken. On: two clock reads a call, which inflates these rows a little.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
extern int port_move_sys_flag;  // 1 while the system timers are on (set at each turn's end, perf.cpp)
void port_move_sys_begin(int id);
void port_move_sys_end(int id);
#ifdef __cplusplus
}

namespace port {
struct MoveSysScope {
    int id;
    explicit MoveSysScope(int which) : id(-1) {
        if (__builtin_expect(__atomic_load_n(&port_move_sys_flag, __ATOMIC_RELAXED) != 0, 0)) {
            id = which;
            port_move_sys_begin(which);
        }
    }
    ~MoveSysScope() {
        if (__builtin_expect(id >= 0, 0)) port_move_sys_end(id);
    }
    MoveSysScope(const MoveSysScope&) = delete;
    MoveSysScope& operator=(const MoveSysScope&) = delete;
};
}  // namespace port
#define PORT_MOVE_SYS(id) ::port::MoveSysScope portMoveSysScope_((id))
#endif
