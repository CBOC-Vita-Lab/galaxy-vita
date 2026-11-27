// polish23 (8 Nov): GXV_PAR_PTCL's storage across many stage entries, counting the C heap. Built by tools/test_par_ptcl_stages.sh on top of
// tests/par_ptcl.cpp (its worlds, resources, events and dump; its own scenario main is left out).
//
// What the game does at every stage entry: GameScene::initEffectSystem makes a JPAEmitterManager (256, 384 or 512 emitters) in the scene's
// heap; ParticleCalcExecutor calls calcNormal every turn, and the first one makes the manager's GXV_PAR_PTCL storage (host heap since
// alodfix23: 41 bytes and one 540-byte work area an emitter, ~297 KB for 512). The star select's manager (0x20 emitters, made once at boot)
// calcs during the first turns of each stage. When the stage goes, GameSystemSceneController::exeDestroyScene calls port_jpa_par_drop with
// "is this manager in the dying scene heap", then the heap goes with no destructor run.
//
// Here: PTCL_STAGES (default 60) stage entries, the three stage sizes in turn, each stage's manager in a serial world (mode 0) and a
// parallel one (mode PTCL_MODE, default 1; PTCL_OVERLAP=1 arms the overlap) compared after every turn, the star select's manager pair
// beside them. At the end of each stage port_jpa_par_drop(dying) runs as the scene destruction does; the managers' own memory is left
// (the scene heap's, not the C heap's), and is measured when made, so what is left of the C heap's growth is the particle storage: it
// must be 0 from the third stage on (glibc's per-thread cache of small freed blocks counts as in use: the script turns it off). PTCL_STAGES_NODROP=1 leaves the drop out (the game before alodfix23's exeDestroyScene hook): the
// check must FAIL with ~41 + 540 bytes an emitter per stage.
#define main par_ptcl_scenarios_main
#include "par_ptcl.cpp"
#undef main
#include <malloc.h>

static const void* gStageMgrS = nullptr;
static const void* gStageMgrP = nullptr;
static int StageDies(const void* m) {
    return m == gStageMgrS || m == gStageMgrP ? 1 : 0;
}
static long long HeapInUse() {
    struct mallinfo2 mi = mallinfo2();
    return static_cast< long long >(mi.uordblks);
}

int main() {
    for (int i = 0; i < 64; ++i) {
        gPrmTbl[i] = GXColor{static_cast< u8 >(i * 4), static_cast< u8 >(255 - i * 3), static_cast< u8 >(i * 7), static_cast< u8 >(100 + i)};
        gEnvTbl[i] = GXColor{static_cast< u8 >(i * 5), static_cast< u8 >(i * 2), static_cast< u8 >(255 - i), 255};
    }
    for (int i = 0; i < 16; ++i) {
        gTexTbl[i] = static_cast< u8 >((i * 5) % 16);
        gTDB1[i] = 0;
    }
    const int kRes = 9;
    ResBuild* rb[kRes];
    for (int k = 0; k < kRes; k++) rb[k] = makeRes(k);
    const int nStages = getenv("PTCL_STAGES") && atoi(getenv("PTCL_STAGES")) > 0 ? atoi(getenv("PTCL_STAGES")) : 60;
    const int pmode = getenv("PTCL_MODE") ? atoi(getenv("PTCL_MODE")) : 1;
    const bool overlapOn = getenv("PTCL_OVERLAP") != nullptr;
    const bool drop = getenv("PTCL_STAGES_NODROP") == nullptr;
    // the star select's pair: made once, never dropped (it is in no scene heap)
    World selS = makeWorld(rb, kRes, 0x300, 0x20);
    World selP = makeWorld(rb, kRes, 0x300, 0x20);
    const unsigned emtrOf[3] = {512, 384, 256};
    const unsigned poolOf[3] = {6144, 5120, 3072};
    std::vector< long long > residual;
    residual.reserve(static_cast< size_t >(nStages));
    gSecs.reserve(1 << 16);
    std::vector< unsigned char > dS, dP;
    dS.reserve(8 << 20);
    dP.reserve(8 << 20);
    int failures = 0;
    unsigned long long parTurns = 0, dropped = 0;
    for (int st = 0; st < nStages && failures == 0; st++) {
        const long long h0 = HeapInUse();
        // the stage's managers (in the game: in the scene heap; here their C heap use is measured and set aside)
        World S = makeWorld(rb, kRes, poolOf[st % 3], emtrOf[st % 3]);
        World P = makeWorld(rb, kRes, poolOf[st % 3], emtrOf[st % 3]);
        const long long managers = HeapInUse() - h0;
        srand(1000u + static_cast< unsigned >(st));
        for (int i = 0; i < 64; i++) gHost[i] = static_cast< float >(rand() % 100);
        Event evs[64], sevs[2];
        for (int turn = 0; turn < 120 && failures == 0; turn++) {
            const bool select = turn < 40;  // (the star select runs during the stage's first turns)
            int ne = 0, ns = 0;
            const int nSpawn = turn < 60 ? 6 : (rand() % 100 < 30 ? 2 : 0);
            for (int i = 0; i < nSpawn; i++) evs[ne++] = Event{0, rand(), rand() % kRes, rand(), 0};
            for (int i = 0; i < 6; i++) evs[ne++] = Event{1 + rand() % 5, rand() % 200, rand(), 0, static_cast< float >(rand() % 200)};
            if (select && turn < 8) sevs[ns++] = Event{0, rand(), rand() % kRes, rand(), 0};
            for (int wi = 0; wi < 2; wi++) {
                for (int i = 0; i < ne; i++) applyEvent(wi ? P : S, rb, evs[i], 0);
                for (int i = 0; i < ns; i++) applyEvent(wi ? selP : selS, rb, sevs[i], 0);
            }
            gSkippedCur = 0;
            memset(gTot, 0, sizeof gTot);
            gMode = 0;
            gWorldP = 0;
            gPortPtclStageCalc = 1;
            S.mgr->calcNormal();
            if (select) {
                gPortPtclStageCalc = 0;
                selS.mgr->calcNormal();
            }
            gMode = pmode;
            gWorldP = 1;
            gPortPtclStageCalc = 1;
            if (overlapOn) port_ptcl_overlap_arm(1);
            P.mgr->calcNormal();
            if (overlapOn) port_ptcl_overlap_arm(0);
            if (select) {
                gPortPtclStageCalc = 0;
                selP.mgr->calcNormal();  // (with the overlap armed above: it finishes the stage's calc first)
            }
            port_ptcl_overlap_finish();
            parTurns += gTot[1];  // (the stage's and the star select's parallel turns: with the overlap their notes come in either order)
            for (int w = 0; w < 2; w++) {
                dump(w ? selS : S, dS);
                dump(w ? selP : P, dP);
                if (dS != dP) {
                    printf("FAIL stage %d turn %d: the %s manager's parallel world differs from the serial one\n", st, turn, w ? "star select" : "stage");
                    failures++;
                    break;
                }
            }
        }
        // the stage ends (GameSystemSceneController::exeDestroyScene): the scene's managers' particle storage goes, the star select's stays
        if (drop) {
            gStageMgrS = S.mgr;
            gStageMgrP = P.mgr;
            dropped += port_jpa_par_drop(&StageDies);
            gStageMgrS = gStageMgrP = nullptr;
            if (P.mgr->mPortPar != nullptr || selP.mgr->mPortPar == nullptr) {
                printf("FAIL stage %d: after the drop the stage's storage is %p, the star select's %p (want none / kept)\n", st,
                       static_cast< void* >(P.mgr->mPortPar), static_cast< void* >(selP.mgr->mPortPar));
                failures++;
            }
        }
        residual.push_back(HeapInUse() - h0 - managers);
        if (st < 4 || st == nStages - 1)
            printf("stage %2d (%u emitters): C heap +%lld bytes for the two managers, +%lld left by the particle storage\n", st, emtrOf[st % 3], managers,
                   residual.back());
    }
    long long late = 0;
    for (size_t i = 2; i < residual.size(); i++) late += residual[i];
    if (failures == 0) {
        printf("particle storage left on the C heap over stages 3..%d: %lld bytes (%.1f a stage); storages dropped %llu; parallel turns %llu\n", nStages,
               late, residual.size() > 2 ? static_cast< double >(late) / static_cast< double >(residual.size() - 2) : 0.0, dropped, parTurns);
        if (late != 0) {
            printf("FAIL the C heap grows with the stage entries\n");
            failures++;
        }
        if (parTurns == 0) {
            printf("FAIL no parallel turn (the check proves nothing)\n");
            failures++;
        }
    }
    printf(failures ? "par_ptcl_stages: FAIL\n" : "par_ptcl_stages: PASS (%d stage entries: no C heap growth, parallel worlds equal the serial ones)\n", nStages);
    return failures ? 1 : 0;
}
