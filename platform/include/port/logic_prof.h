// GXV_LOGIC_PROF (logic23, 8 Nov; default 0; every build; platform/src/port/perf.cpp): where the game thread's "update" and
// "calc-anim" go, by object class and by system, as "logic-prof:" lines in log.txt every 300 turns (~5 s at 60).
//
//   0  off: one relaxed load of port_logic_prof_flag and a branch not taken per hook.
//   1  classes + systems:
//      - every object of a movement or calc-anim category list (NameObjCategoryList::execute) is timed whole and summed per CLASS
//        (its vtable; the row is named by its model for a LiveActor, else by its NameObj name, and shows the vtable address so the
//        integrator can name it with `arm-vita-eabi-nm -C <elf> | grep <addr>`);
//      - the per-turn systems below are timed inclusive (their outermost call per id), with the part of it spent inside the
//        player's (Mario's) lists shown separately;
//      - counters (calls, gravities visited, areas tested, collision parts looked at / searched).
//   2  1, plus the phases of LiveActor::movement (anim clock, gravity, sensor messages, nerve, control, binder, effects, camera
//      controller, light, sensor positions, sound, shadow request): ~24 more clock reads per actor; read the shares.
//   3  (game24, 10 Nov) the SAMPLED profile, cheap enough that its numbers are the real ones, one console run for the whole table:
//      - EVERY turn: each movement and calc-anim category list timed whole (two reads a list), the non-list parts of update
//        (reset process, scene change check, ObjHolder::update's four parts, the audio system, the sequence director, the system wipe,
//        the home / error / dimming part, SceneFunction::executeMovementList's controller / requirement / camera-matrix parts), the
//        scene's update and movement-list totals, GameSystem::update / calcAnim whole: ~110 reads a turn (~0.1 ms);
//      - one turn in GXV_LOGIC_PROF_SAMPLE (default 4): everything of =2 (every object timed per class, the movement phases, the
//        systems, the counts), so the top-10 class table comes from a quarter of the turns, and its inflation is measured and shown
//        (the same lists on the unsampled turns against the sampled ones) so the class rows can be scaled to real ms.
// Every timer read is counted; the cost of one read is measured at each window's start and the rows are printed with the reads
// made inside them taken off ("corrected"), next to the raw total. Rows are game-thread wall time (preemption included).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
extern int port_logic_prof_flag;  // 0 off, 1, 2 (copied from GXV_LOGIC_PROF at each turn's end; =3: 2 on a sampled turn, else 0)
extern int port_logic_prof_coarse;  // 1: the two phases are timed whole (=1, =2, =3); 2: also every category list and update's parts (=3)
void port_logic_prof_obj_begin(const void* pObj, const char* pName);
void port_logic_prof_obj_end(void);
int port_logic_prof_obj_wants_name(void);
void port_logic_prof_obj_name(const char* pModelName);
void port_logic_prof_sys_begin(int id);
void port_logic_prof_sys_end(int id);
void port_logic_prof_count(int id, unsigned n);
void port_logic_prof_turn(int drawn);
void port_logic_prof_bck(const void* pAnm, float frame, unsigned jointNo);  // (=2: a BCK joint evaluation, game thread only)
void port_logic_prof_cat_begin(int raw);  // (=3, every turn: a movement (0x100 | type) or calc-anim (type) category list, timed whole)
void port_logic_prof_cat_end(void);
// (update25, =3 on a sampled turn) the drawn turn's flushes per model: kind 0 = the material lists (port_dl_flush_stale), 1 = the far
// models' joints (port_anim_flush_stale). begin names the owner by its class (vtable) and NameObj name; end: outcome 0 computed, 1 left out.
void port_logic_prof_flush_begin(int kind, const void* pObj, const char* pName);
void port_logic_prof_flush_end(int outcome);
#ifdef __cplusplus
}
#endif

// systems (inclusive; a system called inside another is counted in both)
#define PORT_LS_GRAVITY 0       // PlanetGravityManager::calcTotalGravityVector: every gravity query, the memo's hits included
#define PORT_LS_GRAVITY_WALK 1  // its walk over the gravities (calcTotalGravityVectorRaw: the memo's misses)
#define PORT_LS_AREA_FIND 2     // AreaObjMgr::find_in (named area lookups and the light areas)
#define PORT_LS_AREA_MGR 3      // AreaObjContainer::getManager (the manager looked up by name, before each named lookup)
#define PORT_LS_COLL 4          // CollisionCategorizedKeeper::checkStrike* / createAreaPolygonList (map and object collision queries)
#define PORT_LS_BINDER 5        // Binder::bind (the binder of every actor that has one; includes its collision queries)
#define PORT_LS_CALCANIM 6      // LiveActor::calcAnim (the joints, materials and the light of a model, in the calc-anim lists)
// LiveActor::movement's phases (GXV_LOGIC_PROF=2)
#define PORT_LS_MV_FIRST 7
#define PORT_LS_MV_ANIMCLOCK 7  // ModelManager::update + ActorAnimKeeper::update (BCK clocks, anim switching)
#define PORT_LS_MV_GRAVITY 8    // MR::calcGravity
#define PORT_LS_MV_OBJCOL 9     // HitSensorKeeper::doObjCol (the actor's attackSensor / receiveMessage reactions)
#define PORT_LS_MV_NERVE 10     // Spine::update (the nerve's exe function: most of a class's own logic)
#define PORT_LS_MV_CONTROL 11   // control()
#define PORT_LS_MV_BINDER 12    // updateBinder (bind + the position)
#define PORT_LS_MV_EFFECT 13    // EffectKeeper::update
#define PORT_LS_MV_CAMERA 14    // ActorPadAndCameraCtrl::update
#define PORT_LS_MV_LIGHT 15     // MR::updateLightCtrl (the light-area lookup and the light blend)
#define PORT_LS_MV_SENSORPOS 16 // MR::tryUpdateHitSensorsAll
#define PORT_LS_MV_SOUND 17     // MR::actorSoundMovement
#define PORT_LS_MV_SHADOWREQ 18 // MR::requestCalcActorShadow
// the two phases whole (GameSystem::frameLoop), so the part outside the category lists shows
#define PORT_LS_PH_UPDATE 19    // GameSystem::update (turn-prof's "update")
#define PORT_LS_PH_CALC 20      // GameSystem::calcAnim (turn-prof's calc-anim + shadows + particles + view/rest)
// (game24, GXV_LOGIC_PROF=3, every turn) update's parts outside the actor lists, and the movement-list function's own parts
#define PORT_LS_U_RESET 21      // GameSystemResetAndPowerProcess::movement
#define PORT_LS_U_SCENECHG 22   // SceneController::checkRequestAndChangeScene
#define PORT_LS_U_OBJHOLDER 23  // GameSystemObjHolder::update (its four parts below)
#define PORT_LS_U_HOME 24       // the home button layout, the error watcher and the dimming watcher
#define PORT_LS_U_NERVE 25      // GameSystem::updateNerve whole (updateSceneController: audio, sequences, wipe, the scene)
#define PORT_LS_U_AUDIO 26      // GameSystemObjHolder::updateAudioSystem (JAudio's movement on the game thread)
#define PORT_LS_U_SEQDIR 27     // GameSequenceDirector::update
#define PORT_LS_U_SCENE 28      // SceneController::updateScene (the scene's update: pause nerves, the movement lists, ...)
#define PORT_LS_U_MOVELISTS 29  // SceneFunction::executeMovementList whole (every category list inside it, and the parts below)
#define PORT_LS_U_ML_CTRL 30    // its scene movement controller and the draw-delay disconnects
#define PORT_LS_U_ML_REQ 31     // its connect / disconnect requirement executions (all six calls)
#define PORT_LS_U_ML_CAMMTX 32  // its star pointer camera matrix and loadViewMtx
#define PORT_LS_U_WIPE 33       // the system wipe's movement
#define PORT_LS_U_OH_RHYTHM 34  // ObjHolder::update: the audio system's rhythm
#define PORT_LS_U_OH_WPAD 35    //                    the Wii pad holder (input)
#define PORT_LS_U_OH_ASYNC 36   //                    the function async executor
#define PORT_LS_U_OH_STARPTR 37 //                    the star pointer director
#define PORT_LS_C_LATE 38       // (calc phase) port_par_late_prepare: GXV_PAR_LATE's batch at calc-anim's start
#define PORT_LS_C_PTCLFIN 39    // (calc phase) port_ptcl_overlap_finish: the particle chains' join and the serial rest
#define PORT_LS_C_ANIMDISP 40   // (calc phase) port_par_late_end + port_par_anim_dispatch: par-anim's next batch
#define PORT_LS_N 41
#define PORT_LS_COARSE_FIRST 21
// counters
#define PORT_LC_GRAV_VISITS 0   // gravities looked at by the walks
#define PORT_LC_AREA_TESTS 1    // areas whose isInVolume a find_in called
#define PORT_LC_COLL_PARTS 2    // collision parts a query looked at (in the zones it entered)
#define PORT_LC_COLL_SEARCH 3   // ... of them searched (their bounding sphere met the query)
#define PORT_LC_AREA_GRID 4     // find_in answered through GXV_AREA_GRID's cells
#define PORT_LC_GRAV_GRID 5     // gravity walks made through GXV_GRAV_GRID's cells
#define PORT_LC_BCK_CALLS 6     // (=2) BCK joint evaluations (J3DAnmTransformKey::calcTransform) on the game thread
#define PORT_LC_BCK_REPEATS 7   // (=2) ... of them with the same animation, frame bits and joint as one made earlier in the turn
#define PORT_LC_SND_CALLS 8     // (update25) AudSoundNameConverter::getSoundID asks (a sound asked for by name)
#define PORT_LC_SND_SCAN 9      // (update25) ... the table entries its scan looks at (the cost an index saves)
#define PORT_LC_N 10

#ifdef __cplusplus
namespace port {
struct LogicSysScope {
    int id;
    explicit LogicSysScope(int which, int level) : id(-1) {
        if (__builtin_expect(__atomic_load_n(&port_logic_prof_flag, __ATOMIC_RELAXED) >= level, 0)) {
            id = which;
            port_logic_prof_sys_begin(which);
        }
    }
    ~LogicSysScope() {
        if (__builtin_expect(id >= 0, 0)) port_logic_prof_sys_end(id);
    }
    LogicSysScope(const LogicSysScope&) = delete;
    LogicSysScope& operator=(const LogicSysScope&) = delete;
};
// A coarse system (=3, every turn): timed whenever GXV_LOGIC_PROF=3 is on, sampled turn or not.
struct LogicCoarseScope {
    int id;
    explicit LogicCoarseScope(int which) : id(-1) {
        // (the two phases whole run at every level; the parts of update outside the lists only with =3)
        if (__builtin_expect(__atomic_load_n(&port_logic_prof_coarse, __ATOMIC_RELAXED) >= (which >= PORT_LS_COARSE_FIRST ? 2 : 1), 0)) {
            id = which;
            port_logic_prof_sys_begin(which);
        }
    }
    ~LogicCoarseScope() {
        if (__builtin_expect(id >= 0, 0)) port_logic_prof_sys_end(id);
    }
    LogicCoarseScope(const LogicCoarseScope&) = delete;
    LogicCoarseScope& operator=(const LogicCoarseScope&) = delete;
};
struct LogicCatScope {  // one category list, timed whole (=3)
    bool on;
    explicit LogicCatScope(int raw) : on(__builtin_expect(__atomic_load_n(&port_logic_prof_coarse, __ATOMIC_RELAXED) >= 2, 0)) {
        if (on) port_logic_prof_cat_begin(raw);
    }
    ~LogicCatScope() {
        if (__builtin_expect(on, 0)) port_logic_prof_cat_end();
    }
    LogicCatScope(const LogicCatScope&) = delete;
    LogicCatScope& operator=(const LogicCatScope&) = delete;
};
inline bool LogicProfOn(int level = 1) {
    return __builtin_expect(__atomic_load_n(&port_logic_prof_flag, __ATOMIC_RELAXED) >= level, 0);
}
}  // namespace port
#define PORT_LOGIC_CAT2(a, b) a##b
#define PORT_LOGIC_CAT(a, b) PORT_LOGIC_CAT2(a, b)
// A system timed for the rest of the enclosing block (level 1).
#define PORT_LOGIC_SYS(id) ::port::LogicSysScope PORT_LOGIC_CAT(portLogicSys_, __LINE__)((id), 1)
// A part of update outside the actor lists, timed every turn with GXV_LOGIC_PROF=3.
#define PORT_LOGIC_COARSE(id) ::port::LogicCoarseScope PORT_LOGIC_CAT(portLogicCoarse_, __LINE__)((id))
// A phase of LiveActor::movement: `stmt` timed with GXV_LOGIC_PROF=2, plain otherwise.
#define PORT_LOGIC_PHASE(id, stmt)                                \
    do {                                                          \
        ::port::LogicSysScope portLogicPhase_((id), 2);           \
        stmt;                                                     \
    } while (0)
#define PORT_LOGIC_COUNT(id, n)                                   \
    do {                                                          \
        if (::port::LogicProfOn()) port_logic_prof_count((id), (n)); \
    } while (0)
#endif
