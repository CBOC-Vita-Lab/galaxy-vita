// The real quiet-player runtime entry points with deterministic clocks.
#include <cstdint>
#include "../platform/src/port/perf.cpp"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

static int64_t Wall = 1000000000, Cpu = 1000;
static unsigned WallReads = 0, CpuReads = 0;
static int Decision = 0;
static bool ClobberErrno = false;
static bool InjectReportCommit = false;
static bool InjectMaterialReportCommit = false;
static std::vector<std::string> Logs;
#if defined(WORKLOAD_TEST_EMUTLS)
static std::atomic<unsigned> TlsLookups{0};
extern "C" void* __real___emutls_get_address(void*);
extern "C" void* __wrap___emutls_get_address(void* p) {
    ++TlsLookups;
    void* result = __real___emutls_get_address(p);
    errno = EDOM;
    return result;
}
#endif
extern "C" int64_t port_host_time_ns() { ++WallReads; if (ClobberErrno) errno = EDOM; return Wall; }
#if defined(__vita__)
extern "C" int64_t port_thread_cpu_us() { ++CpuReads; return Cpu; }
#endif

#if defined(__vita__) && PORT_LOGGING && !defined(VGX_ANIM_MATERIAL_VERSION)
extern "C" void port_workload_anim_material_checkpoint(uint32_t,unsigned) {}
extern "C" uint32_t port_workload_anim_material_diff_context() { return 0; }
extern "C" void port_workload_anim_material_diff_counts(uint32_t,uint32_t,uint32_t,uint32_t) {}
#endif
extern "C" void port_log_summary(const char* fmt, ...) {
    char text[2048]; va_list args; va_start(args, fmt); std::vsnprintf(text, sizeof text, fmt, args); va_end(args);
    Logs.emplace_back(text);
#if defined(VGX_ANIM_FAMILIES_VERSION) && defined(__vita__) && PORT_LOGGING
    if (InjectReportCommit && std::string(text).find("workload-anim: nonview ") == 0) {
        InjectReportCommit = false;
        port_perf_frame_begin();
        const auto p=port_workload_anim_begin(), c=port_workload_anim_category_begin(0,6), f=port_workload_anim_family_begin(0);
        Wall+=2000; port_workload_anim_family_end(f); port_workload_anim_category_end(c); port_workload_anim_end(p);
        port_perf_frame_work_done();
    }
#endif
#if defined(VGX_ANIM_MATERIAL_VERSION) && defined(__vita__) && PORT_LOGGING
    if (InjectMaterialReportCommit && std::string(text).find("workload-anim: nonview ") == 0) {
        InjectMaterialReportCommit = false; port_perf_frame_begin();
        const auto p=port_workload_anim_begin(),c=port_workload_anim_category_begin(0,6),f=port_workload_anim_family_begin(2);
        port_workload_anim_material_checkpoint(f,1);port_workload_anim_material_checkpoint(f,2);port_workload_anim_material_checkpoint(f,4);
        port_workload_anim_material_checkpoint(f,3);port_workload_anim_material_diff_counts(port_workload_anim_material_diff_context(),1,1,1);
        Wall+=2000;port_workload_anim_material_checkpoint(f,4);port_workload_anim_family_end(f);port_workload_anim_category_end(c);port_workload_anim_end(p);port_perf_frame_work_done();
    }
#endif
}
extern "C" void port_log(const char*, ...) {}
extern "C" int port_handheld() { return 1; }
extern "C" unsigned port_vgx_backend_pending() { return 0; }
extern "C" int port_vgx_pace_turn(int) { return Decision; }
extern "C" void port_autoq_game_begin() {}
extern "C" void port_autoq_game_work_done() {}
extern "C" void port_settings_panel_turn(int) {}
namespace MR { const char* getCurrentStageName() { return ""; } }
static void require(bool yes, const char* why) {
    if (!yes) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

#if defined(__vita__) && PORT_LOGGING && !defined(VGX_ANIM_CATEGORIES_VERSION)
// Baseline-only adapters let the first behavioral assertion run before the feature exists.
extern "C" uint32_t port_workload_anim_begin() { return 0; }
extern "C" void port_workload_anim_end(uint32_t) {}
extern "C" uint32_t port_workload_anim_category_begin(unsigned,unsigned) { return 0; }
extern "C" void port_workload_anim_category_end(uint32_t) {}
#endif

#if defined(__vita__) && PORT_LOGGING && !defined(VGX_ANIM_FAMILIES_VERSION)
extern "C" uint32_t port_workload_anim_family_begin(unsigned) { return 0; }
extern "C" void port_workload_anim_family_end(uint32_t) {}
#endif

#if defined(VGX_ANIM_CATEGORIES_VERSION) && defined(__vita__) && PORT_LOGGING
using namespace vgx::workload;
static AnimSnapshot snapshot() { AnimSnapshot s; require(ReadAnimSnapshot(s), "stable animation snapshot"); return s; }
static void beginTurn(int decision = -1) {
    port_perf_frame_begin();
    if (decision >= 0) { Decision = decision; require(port_frameskip_turn(0) == decision, "actual frame decision retained"); }
}
static void endTurn() { port_perf_frame_work_done(); }
static void noCpu(const AnimSnapshot& s) {
    for (const auto& r : s.rows) require(!r.cpuSamples && !r.cpuUs && !r.matchedWallUs, "animation rows stay wall-only");
}
static void timing() {
    beginTurn(0);
    const unsigned cpu = CpuReads, wall = WallReads;
    ClobberErrno = true; errno = E2BIG;
    const auto p = port_workload_anim_begin(); require(p != 0, "selected context gets a ticket");
    Wall += 1000000;
    const auto c = port_workload_anim_category_begin(0, 4); require(c != 0, "environment category ticket");
    Wall += 2000000;
    const auto nested = port_workload_anim_category_begin(0, 4); require(nested != 0 && nested != c, "same-ID nesting has distinct ticket");
    port_workload_anim_category_end(0);
    Wall += 3000000; port_workload_anim_category_end(nested); port_workload_anim_category_end(c);
    const auto u = port_workload_anim_category_begin(99, 4); Wall += 1000000; port_workload_anim_category_end(u);
    Wall += 3000000; port_workload_anim_end(0); port_workload_anim_end(p);
    require(errno == E2BIG, "enabled TLS and clocks preserve errno");
    ClobberErrno = false;
    require(CpuReads == cpu && WallReads - wall == 6, "only parent and top-level categories read wall endpoints");
    require(snapshot().rows[AnimParent].calls == 0, "no partially committed turn is visible");
    endTurn(); const auto s = snapshot(); noCpu(s);
    require(s.rows[AnimParent].wallUs == 10000 && s.rows[AnimParent].calls == 1, "exact non-view parent");
    require(s.rows[AnimEnvironment].wallUs == 5000 && s.rows[AnimEnvironment].calls == 1, "nested work belongs only to owning top-level category");
    require(s.rows[AnimUnknown].wallUs == 1000 && s.rows[AnimUnknown].calls == 1 && s.rows[AnimResidual].wallUs == 4000, "unknown label and explicit residual");
    require(s.census[AnimSelected] == 1 && s.census[AnimDrawn] == 1 && s.census[AnimNested] == 1, "selected drawn and nesting census");
    require(s.census[AnimComplete] == 1 && s.census[AnimUnknownDispatch] == 1, "complete and unknown dispatch census");
    WindowStart = Wall; for (unsigned i=0;i<LaneCount;++i) Previous[i]=Counters[i].Read();
    Wall += 5000000000ll; Report();
    bool parent=false, header=false, zero=false;
    for (const auto& l : Logs) {
        parent |= l.find("workload-anim: nonview calls=1 wall_samples=1 wall_us=10000 cpu_samples=0") != std::string::npos;
        header |= l.find("workload-anim: stride=1 selected_turns=1 drawn=1 skipped=0 unknown=0") != std::string::npos;
        zero |= l.find("workload-anim: movie-subtitles calls=0") != std::string::npos;
    }
    require(parent && header && zero, "basic report includes coherent counters and zero-call known rows");
    AnimSequence.fetch_add(1); AnimSnapshot sentinel; sentinel.rows[AnimParent].calls=123;
    require(!ReadAnimSnapshot(sentinel) && sentinel.rows[AnimParent].calls==123, "odd publication never leaks a snapshot");
    const auto old = AnimPrevious.rows[AnimParent].calls; Wall += 5000000000ll; Logs.clear(); Report();
    require(AnimPrevious.rows[AnimParent].calls==old, "unstable report retains animation baseline");
    for(const auto& l:Logs) require(l.find("workload-anim:")==std::string::npos,"unstable animation report is skipped");
    AnimSequence.fetch_add(1);
}
static void malformed() {
    beginTurn(2); const auto p=port_workload_anim_begin();
    std::thread foreign([&] { errno=E2BIG; require(port_workload_anim_begin()==0,"foreign context ignored"); port_workload_anim_end(p); port_workload_anim_category_end(p); require(errno==E2BIG,"foreign first-use TLS preserves errno"); }); foreign.join();
    const auto c=port_workload_anim_category_begin(1,45); Wall+=1234567;
    port_workload_anim_category_end(p); // Wrong ticket cannot end the live category.
    port_workload_anim_category_end(c); Wall+=2000000; port_workload_anim_end(p); endTurn();
    auto s=snapshot(); require(s.rows[AnimShadow].calls==1 && s.census[AnimSkipped]==1 && s.census[AnimOrphan]==1,"typed shadow and orphan end census");
    beginTurn(); const auto stale=port_workload_anim_begin(); (void)port_workload_anim_category_begin(0,6); endTurn();
    require(snapshot().census[AnimUnfinished]==1,"unfinished parent counted separately");
    beginTurn(); const auto next=port_workload_anim_begin(); port_workload_anim_end(stale); Wall+=5000; port_workload_anim_end(next); endTurn();
    s=snapshot(); require(s.rows[AnimParent].calls==2 && s.census[AnimUnknownTurn]==2,"stale token cannot close next generation; unknown is not drawn");
    beginTurn(0); const auto bad=port_workload_anim_begin(); require(port_workload_anim_begin()==0,"nested context rejects conservatively"); Wall+=1000; port_workload_anim_end(bad); endTurn();
    s=snapshot(); require(s.census[AnimInvalid]==1 && s.rows[AnimParent].calls==2,"nested context publishes no partition");
    beginTurn(); const auto b=port_workload_anim_begin(); auto child=port_workload_anim_category_begin(0,4);
    for(unsigned i=0;i<40;++i) (void)port_workload_anim_category_begin(0,4);
    port_workload_anim_category_end(child); port_workload_anim_end(b); endTurn();
    require(snapshot().census[AnimDepthOverflow]==1,"bounded nested-stack overflow is explicit");
    beginTurn(); const auto rollback=port_workload_anim_begin(); Wall-=1; port_workload_anim_end(rollback); endTurn();
    require(snapshot().census[AnimInvalid]==3,"backwards-clock parent cannot publish a partition");
    beginTurn(); const auto missing=port_workload_anim_begin(); (void)port_workload_anim_category_begin(0,4); port_workload_anim_end(missing); endTurn();
    require(snapshot().census[AnimInvalid]==4,"unclosed child invalidates completed parent");
    beginTurn(); const auto partial=port_workload_anim_begin(); (void)partial;
    beginTurn(); Wall+=1000; const auto valid=port_workload_anim_begin(); Wall+=1000; port_workload_anim_end(valid); endTurn();
    require(snapshot().census[AnimUnfinished]==2,"next turn closes abandoned publication without stale reuse");
    const auto before=snapshot();
    std::thread outsider([&]{ beginTurn(0); require(port_workload_anim_begin()==0,"another thread with its own turn is not the main profiling owner"); endTurn(); }); outsider.join();
    require(snapshot().census[AnimSelected]==before.census[AnimSelected],"foreign turn cannot publish into owner partition");
    noCpu(snapshot());
}
static void sampling() {
    for(unsigned i=0;i<63;++i) {
        beginTurn(i%4==0?0:2); const unsigned wall=WallReads,cpu=CpuReads;
        const auto p=port_workload_anim_begin(); require((p!=0)==(i%31==0),"whole-turn stride31 selection independent of ProcessStream");
        const auto c=port_workload_anim_category_begin(0,4); Wall+=1500; port_workload_anim_category_end(c); Wall+=1500; port_workload_anim_end(p);
        require(CpuReads==cpu && WallReads-wall==(p?4u:0u),"unselected category turn reads no clocks"); endTurn();
    }
    const auto s=snapshot(); require(s.census[AnimSelected]==3 && s.census[AnimDrawn]==1 && s.census[AnimSkipped]==2,"selected actual cadence samples cross drawn/skipped turns");
    require(s.rows[AnimParent].wallUs==9 && s.rows[AnimEnvironment].wallUs==3 && s.rows[AnimResidual].wallUs==6,"microsecond residual includes category truncation and partitions exactly");
}
static void labels() {
    constexpr unsigned ids[]={4,5,6,7,8,9,10,11,12,13,14,15,16,18,19,20,45};
    beginTurn(); const auto p=port_workload_anim_begin();
    for(unsigned id:ids) { const auto c=port_workload_anim_category_begin(id==45?1:0,id); Wall+=1000; port_workload_anim_category_end(c); }
    port_workload_anim_end(p); endTurn(); const auto s=snapshot();
    for(unsigned row=AnimEnvironment;row<AnimUnknown;++row) require(s.rows[row].calls==1 && s.rows[row].wallUs==1,"all seventeen typed union categories retain their own row");
    require(s.rows[AnimUnknown].calls==0 && s.rows[AnimParent].wallUs==17 && s.rows[AnimResidual].wallUs==0,"known label union has no hidden work or unknown labels");
}
static void classification() {
    beginTurn(); // Fast-forward/no decision: two real contexts are not two selected turns.
    for(unsigned i=0;i<2;++i) {
        const auto p=port_workload_anim_begin(); const auto c=port_workload_anim_category_begin(0,4);
        Wall+=1000; port_workload_anim_category_end(c); Wall+=2000; port_workload_anim_end(p);
    }
    endTurn(); auto s=snapshot();
    require(s.census[AnimSelected]==1 && s.census[AnimUnknownTurn]==1 && s.census[AnimComplete]==2,"multiple contexts in unknown fast-forward turn retain the turn denominator");
    require(s.rows[AnimParent].calls==2 && s.rows[AnimParent].wallUs==6 && s.rows[AnimEnvironment].wallUs==2 && s.rows[AnimResidual].wallUs==4,
            "multiple complete parent partitions accumulate without leaking sibling state");
    beginTurn(0); Decision=2; (void)port_frameskip_turn(0); Decision=0; (void)port_frameskip_turn(0); endTurn();
    s=snapshot(); require(s.census[AnimUnknownTurn]==2 && !s.census[AnimDrawn] && !s.census[AnimSkipped],"conflicting actual decisions stay unknown even if a later call agrees again");
    beginTurn(0); (void)port_frameskip_turn(0); endTurn();
    require(snapshot().census[AnimDrawn]==1,"repeated equal actual decisions remain drawn");
}
static void concurrent() {
    beginTurn(); endTurn(); // Claim the production main-thread publisher before starting a reader.
    std::atomic<bool> done{false}; std::atomic<unsigned> reads{0};
    std::thread reader([&] {
        do {
            AnimSnapshot s;
            if(ReadAnimSnapshot(s)) {
                const auto n=s.rows[AnimParent].calls;
                require(s.rows[AnimParent].wallUs==n*5 && s.rows[AnimEnvironment].wallUs==n*2 && s.rows[AnimResidual].wallUs==n*3,
                        "concurrent reader sees an exact complete-parent partition, never a midcommit mix");
                require(s.census[AnimComplete]==n,"coherent parent census matches partition rows"); ++reads;
            }
        } while(!done.load());
    });
    for(unsigned i=0;i<2000;++i) {
        beginTurn(2); const auto p=port_workload_anim_begin(); const auto c=port_workload_anim_category_begin(0,4);
        Wall+=2000; port_workload_anim_category_end(c); Wall+=3000; port_workload_anim_end(p); endTurn();
    }
    done=true; reader.join(); require(reads.load()!=0,"concurrent snapshot reader actually ran");
}
static void overflow() {
    beginTurn(); AnimTickets.store(UINT32_MAX-1); const auto p=port_workload_anim_begin(); require(p==UINT32_MAX-1,"last nonwrapping ticket may be used");
    require(port_workload_anim_category_begin(0,4)==0,"ticket exhaustion disables child without reuse");
    port_workload_anim_end(p); endTurn(); auto s=snapshot();
    require(s.census[AnimTicketOverflow]==1 && !s.rows[AnimParent].calls,"ticket overflow invalidates parent, not an attractive residual");
    beginTurn(); require(port_workload_anim_begin()==0,"tickets remain exhausted across turns"); endTurn();
    require(AnimTickets.load()==UINT32_MAX,"ticket cursor saturates rather than wraps through zero");
}
static void familyDisabled() {
    const unsigned wall=WallReads,cpu=CpuReads;
#if defined(WORKLOAD_TEST_EMUTLS)
    const unsigned tls=TlsLookups;
#endif
    ClobberErrno=true; errno=E2BIG;
    require(port_workload_anim_family_begin(0)==0,"disabled family observer returns inactive ticket");
    port_workload_anim_family_end(123); port_workload_anim_family_end(0);
    require(WallReads==wall && CpuReads==cpu && errno==E2BIG,"family gate precedes clocks and errno effects");
#if defined(WORKLOAD_TEST_EMUTLS)
    require(TlsLookups==tls,"family gate precedes every emulated TLS lookup even with category observer enabled");
#endif
}
#if defined(VGX_ANIM_FAMILIES_VERSION)
static void checkFamilies(const AnimSnapshot& s) {
    for(const auto& row:s.families) require(!row.cpuSamples && !row.cpuUs && !row.matchedWallUs,"families never read CPU clocks");
    for(unsigned target=0;target<2;++target) {
        const auto& cat=s.rows[target?AnimMapObj:AnimNpc]; const unsigned b=target*4;
        require(cat.wallUs==s.families[b].wallUs+s.families[b+1].wallUs+s.families[b+2].wallUs+s.families[b+3].wallUs,
                "family partition uses exact existing category microseconds");
        require(s.families[b+3].calls==cat.calls,"residual count equals complete category count");
    }
}
static void familyTiming() {
    beginTurn(0); const unsigned cpu=CpuReads,wall=WallReads;
    ClobberErrno=true; errno=E2BIG;
    require(port_workload_anim_family_begin(0)==0,"families outside parent ignored");
    const auto p=port_workload_anim_begin(), n=port_workload_anim_category_begin(0,6);
    const auto base=port_workload_anim_family_begin(0); require(base!=0,"sampled NPC base family gets a ticket");
    Wall+=1000; const auto innerCategory=port_workload_anim_category_begin(0,5);
    const auto inner=port_workload_anim_family_begin(1); require(inner && inner!=base,"cross-family nested ticket is distinct");
    const auto same=port_workload_anim_family_begin(0); require(same && same!=base,"same-ID nested family cannot reuse its owner ticket");
    port_workload_anim_family_end(0); Wall+=2000; port_workload_anim_family_end(same); port_workload_anim_family_end(inner);
    port_workload_anim_category_end(innerCategory); port_workload_anim_family_end(base);
    const auto model=port_workload_anim_family_begin(1); Wall+=4000; port_workload_anim_family_end(model);
    const auto material=port_workload_anim_family_begin(2); Wall+=5000; port_workload_anim_family_end(material);
    Wall+=1000; port_workload_anim_category_end(n);
    const auto map=port_workload_anim_category_begin(0,5), m=port_workload_anim_family_begin(1);
    Wall+=2000; port_workload_anim_family_end(m); Wall+=2000; port_workload_anim_category_end(map);
    const auto other=port_workload_anim_category_begin(0,4);
    require(port_workload_anim_family_begin(0)==0,"non-NPC/map root families are excluded");
    Wall+=1000; port_workload_anim_category_end(other); port_workload_anim_end(p);
    require(errno==E2BIG,"enabled family TLS and wall clocks preserve errno"); ClobberErrno=false;
    require(CpuReads==cpu && WallReads-wall==16,"only parent/category and outer-family wall endpoints are read");
    endTurn(); auto s=snapshot(); checkFamilies(s);
    require(s.rows[AnimNpc].wallUs==13 && s.rows[AnimMapObj].wallUs==4,"shared category measurements retained");
    require(s.families[FamilyNpcBase].wallUs==3 && s.families[FamilyNpcModel].wallUs==4 && s.families[FamilyNpcMaterial].wallUs==5 && s.families[FamilyNpcResidual].wallUs==1,
            "NPC disjoint families and unhooked residual");
    require(s.families[FamilyMapModel].wallUs==2 && s.families[FamilyMapResidual].wallUs==2 && !s.families[FamilyMapBase].calls,"map work is not mixed with NPC work");
    require(s.familyCensus[FamilyNested]==2 && !s.families[FamilyMapBase].calls,"nested categories/families remain in original NPC outer owner");
    WindowStart=Wall; Wall+=5000000000ll; InjectReportCommit=true; Report();
    bool total=false,baseRow=false;
    for(const auto& l:Logs) {
        total|=l.find("workload-anim-family: npc-total calls=1 wall_samples=1 wall_us=13 cpu_samples=0")!=std::string::npos;
        baseRow|=l.find("workload-anim-family: npc-base calls=1 wall_samples=1 wall_us=3 cpu_samples=0")!=std::string::npos;
    }
    require(total && baseRow,"family report uses same snapshot despite a producer commit while category rows print");
    require(snapshot().rows[AnimNpc].calls==2 && AnimPrevious.rows[AnimNpc].calls==1 && AnimPrevious.families[FamilyNpcBase].wallUs==3,
            "category and family baseline advance together exactly once");
    Logs.clear(); Wall+=5000000000ll; Report(); bool carried=false;
    for(const auto& l:Logs) carried|=l.find("workload-anim-family: npc-base calls=1 wall_samples=1 wall_us=2 cpu_samples=0")!=std::string::npos;
    require(carried,"later family delta retains intervening completed parent");
}
static void familyMalformed() {
    beginTurn(); auto p=port_workload_anim_begin(),c=port_workload_anim_category_begin(0,6),f=port_workload_anim_family_begin(0);
    std::thread foreign([&]{ errno=E2BIG; require(!port_workload_anim_family_begin(0),"foreign family ignored"); port_workload_anim_family_end(f); require(errno==E2BIG,"foreign first family TLS preserves errno"); }); foreign.join();
    port_workload_anim_family_end(p); Wall+=1000; port_workload_anim_family_end(f); port_workload_anim_category_end(c); port_workload_anim_end(p); endTurn();
    require(snapshot().familyCensus[FamilyOrphan]==1 && snapshot().rows[AnimParent].calls==1,"wrong/foreign ticket cannot close owning family");
    beginTurn(); p=port_workload_anim_begin(); c=port_workload_anim_category_begin(0,6); f=port_workload_anim_family_begin(1);
    port_workload_anim_category_end(c); port_workload_anim_end(p); endTurn();
    require(snapshot().census[AnimInvalid]==1 && snapshot().familyCensus[FamilyUnfinished]==1,"unclosed family invalidates complete parent, not residual");
    beginTurn(); p=port_workload_anim_begin(); c=port_workload_anim_category_begin(0,5); const auto current=port_workload_anim_family_begin(2);
    port_workload_anim_family_end(f); Wall+=1500; port_workload_anim_family_end(current); port_workload_anim_category_end(c); port_workload_anim_end(p); endTurn();
    auto s=snapshot(); checkFamilies(s); require(s.rows[AnimMapObj].calls==1 && s.families[FamilyMapMaterial].wallUs==1,"stale cross-generation family cannot close current family");
    beginTurn(); p=port_workload_anim_begin(); (void)port_workload_anim_category_begin(0,6); (void)port_workload_anim_family_begin(0); endTurn();
    require(snapshot().census[AnimUnfinished]==1 && snapshot().familyCensus[FamilyUnfinished]==2,"abandoned family/context is explicit and discarded");
    beginTurn(); p=port_workload_anim_begin(); c=port_workload_anim_category_begin(0,6); (void)port_workload_anim_family_begin(0);
    for(unsigned i=0;i<20;++i)(void)port_workload_anim_family_begin(0);
    port_workload_anim_category_end(c); port_workload_anim_end(p); endTurn();
    require(snapshot().familyCensus[FamilyDepthOverflow]==1,"family stack overflow is bounded and explicit");
    beginTurn(); p=port_workload_anim_begin(); c=port_workload_anim_category_begin(0,6); require(!port_workload_anim_family_begin(99),"unknown family rejected");
    port_workload_anim_category_end(c); port_workload_anim_end(p); endTurn();
    require(snapshot().familyCensus[FamilyUnknown]==1,"unknown family cannot hide in residual");
    beginTurn(); p=port_workload_anim_begin(); c=port_workload_anim_category_begin(0,6); f=port_workload_anim_family_begin(0); Wall-=1;
    port_workload_anim_family_end(f); port_workload_anim_category_end(c); port_workload_anim_end(p); endTurn();
    require(snapshot().familyCensus[FamilyClockInvalid]==1,"family clock rollback invalidates parent");
    const auto before=snapshot();
    beginTurn(); p=port_workload_anim_begin(); c=port_workload_anim_category_begin(0,6); f=port_workload_anim_family_begin(0);
    Wall+=5000; port_workload_anim_family_end(f); port_workload_anim_category_end(c);
    c=port_workload_anim_category_begin(0,5); (void)port_workload_anim_family_begin(1);
    port_workload_anim_category_end(c); port_workload_anim_end(p); endTurn();
    s=snapshot(); require(s.rows[AnimNpc].wallUs==before.rows[AnimNpc].wallUs && s.families[FamilyNpcBase].wallUs==before.families[FamilyNpcBase].wallUs,
                        "bad later sibling discards earlier complete category and family staging in that parent");
    beginTurn(); (void)port_workload_anim_begin(); (void)port_workload_anim_category_begin(0,6); (void)port_workload_anim_family_begin(0);
    beginTurn(); endTurn();
    require(snapshot().census[AnimUnfinished]==2,"new turn abandons unfinished family parent without reusing staging");
    beginTurn(); p=port_workload_anim_begin(); c=port_workload_anim_category_begin(0,6); AnimTickets.store(UINT32_MAX);
    require(!port_workload_anim_family_begin(0),"family ticket exhaustion does not wrap"); port_workload_anim_category_end(c); port_workload_anim_end(p); endTurn();
    s=snapshot(); require(s.familyCensus[FamilyTicketOverflow]==1 && s.rows[AnimParent].calls==2,"all malformed family parents discard staged rows"); checkFamilies(s);
}
static void familySampling() {
    for(unsigned i=0;i<32;++i) {
        beginTurn(); const unsigned wall=WallReads,cpu=CpuReads; const auto p=port_workload_anim_begin(),c=port_workload_anim_category_begin(0,6),f=port_workload_anim_family_begin(0);
        require((f!=0)==(i%31==0),"family selection is the existing whole-turn sample");
        Wall+=1500; port_workload_anim_family_end(f); Wall+=1500; port_workload_anim_category_end(c); port_workload_anim_end(p);
        require(CpuReads==cpu && WallReads-wall==(p?6u:0u),"unselected families have no clocks"); endTurn();
    }
    const auto s=snapshot();checkFamilies(s);require(s.families[FamilyNpcBase].wallUs==2 && s.families[FamilyNpcResidual].wallUs==4,"family microsecond partition includes rounding residual");
}
static void familyRounding() {
    beginTurn(); const auto p=port_workload_anim_begin();
    auto c=port_workload_anim_category_begin(0,6);
    for(unsigned family=0;family<3;++family) { const auto f=port_workload_anim_family_begin(family); Wall+=1500; port_workload_anim_family_end(f); }
    port_workload_anim_category_end(c); c=port_workload_anim_category_begin(0,6);
    const auto f=port_workload_anim_family_begin(0); Wall+=1500; port_workload_anim_family_end(f); port_workload_anim_category_end(c);
    port_workload_anim_end(p); endTurn();const auto s=snapshot();checkFamilies(s);
    require(s.rows[AnimNpc].calls==2 && s.rows[AnimNpc].wallUs==6 && s.families[FamilyNpcBase].calls==2 && s.families[FamilyNpcBase].wallUs==3,
            "family ns aggregate across repeated same-category roots before conversion");
    require(s.families[FamilyNpcModel].wallUs==1 && s.families[FamilyNpcMaterial].wallUs==1 && s.families[FamilyNpcResidual].wallUs==1,
            "per-category rounding residual matches the already emitted category total exactly");
}
static void familyConcurrent() {
    beginTurn(); endTurn();
    std::atomic<bool> done{false}; std::atomic<unsigned> reads{0};
    std::thread reader([&] {
        do {
            AnimSnapshot s;
            if(ReadAnimSnapshot(s)) {
                checkFamilies(s); const auto n=s.rows[AnimNpc].calls;
                require(s.rows[AnimNpc].wallUs==n*5 && s.families[FamilyNpcBase].wallUs==n*2 && s.families[FamilyNpcModel].wallUs==n && s.families[FamilyNpcMaterial].wallUs==n && s.families[FamilyNpcResidual].wallUs==n,
                        "concurrent category/family payload is one coherent transaction"); ++reads;
            }
        } while(!done.load());
    });
    for(unsigned i=0;i<2000;++i) {
        beginTurn(); const auto p=port_workload_anim_begin(),c=port_workload_anim_category_begin(0,6);
        for(unsigned family=0;family<3;++family) { const auto f=port_workload_anim_family_begin(family); Wall+=family?1000:2000; port_workload_anim_family_end(f); }
        Wall+=1000; port_workload_anim_category_end(c); port_workload_anim_end(p); endTurn();
    }
    done=true;reader.join();require(reads.load()!=0,"concurrent family reader actually ran");
}
#endif
static void familyScenario(const std::string& mode) {
#if defined(VGX_ANIM_FAMILIES_VERSION)
    if(mode=="family-timing") familyTiming(); else if(mode=="family-malformed") familyMalformed(); else if(mode=="family-sampling") familySampling();
    else if(mode=="family-rounding") familyRounding(); else if(mode=="family-concurrent") familyConcurrent(); else familyDisabled();
#else
    (void)mode; beginTurn(); (void)port_workload_anim_begin(); (void)port_workload_anim_category_begin(0,6);
    require(port_workload_anim_family_begin(0)!=0,"sampled NPC family must receive an active ticket");
#endif
}
static void materialDisabled() {
    const unsigned wall=WallReads,cpu=CpuReads;
#if defined(WORKLOAD_TEST_EMUTLS)
    const unsigned tls=TlsLookups;
#endif
    ClobberErrno=true;errno=E2BIG;
    port_workload_anim_material_checkpoint(123,1);port_workload_anim_material_checkpoint(0,99);
    require(!port_workload_anim_material_diff_context(),"disabled material census has no context");
    port_workload_anim_material_diff_counts(123,5,3,3);port_workload_anim_material_diff_counts(0,5,3,3);
    require(WallReads==wall && CpuReads==cpu && errno==E2BIG,"material missing-opt-in gate precedes clocks/errno effects");
#if defined(WORKLOAD_TEST_EMUTLS)
    require(TlsLookups==tls,"material gate precedes every observer emulated TLS lookup");
#endif
}
#if defined(VGX_ANIM_MATERIAL_VERSION)
static void checkMaterial(const AnimSnapshot& s) {
    checkFamilies(s);
    for(const auto& r:s.materials)require(!r.cpuSamples && !r.cpuUs && !r.matchedWallUs,"material phases stay CPU-zero");
    for(unsigned target=0;target<2;++target) {
        uint32_t sum=0;for(unsigned k=0;k<5;++k)sum+=s.materials[target*5+k].wallUs;
        require(sum==s.families[target*4+2].wallUs,"five phases exactly equal same material family row");
    }
}
static void materialTiming() {
    beginTurn();const unsigned wall=WallReads,cpu=CpuReads;ClobberErrno=true;errno=E2BIG;
    const auto p=port_workload_anim_begin(),c=port_workload_anim_category_begin(0,6),f=port_workload_anim_family_begin(2);
    Wall+=1000;port_workload_anim_material_checkpoint(f,1);Wall+=2000;port_workload_anim_material_checkpoint(f,2);
    Wall+=3000;port_workload_anim_material_checkpoint(f,4);Wall+=4000;port_workload_anim_material_checkpoint(f,3);
    const unsigned reads=WallReads;const auto context=port_workload_anim_material_diff_context();require(context==f,"diff census uses outer material token");
    port_workload_anim_material_diff_counts(context,5,3,3);port_workload_anim_material_diff_counts(context,2,1,1);
    require(WallReads==reads,"aggregate diff census has no per-material or batch clock");
    Wall+=5000;port_workload_anim_material_checkpoint(f,4);Wall+=6000;port_workload_anim_family_end(f);port_workload_anim_category_end(c);
    const auto map=port_workload_anim_category_begin(0,5),mf=port_workload_anim_family_begin(2);
    require(!port_workload_anim_material_diff_context(),"census outside tracked diff phase ignored");
    Wall+=1000;port_workload_anim_material_checkpoint(mf,1);Wall+=2000;port_workload_anim_material_checkpoint(mf,2);
    Wall+=3000;port_workload_anim_material_checkpoint(mf,4);Wall+=4000;port_workload_anim_family_end(mf);port_workload_anim_category_end(map);port_workload_anim_end(p);
    require(CpuReads==cpu && WallReads-wall==18 && errno==E2BIG,"checkpoints are wall-only, reuse family endpoints and preserve errno");ClobberErrno=false;
    endTurn();auto s=snapshot();checkMaterial(s);
    require(s.materials[MaterialNpcEntryLock].wallUs==1 && s.materials[MaterialNpcPrepare].wallUs==2 && s.materials[MaterialNpcCalc].wallUs==3 && s.materials[MaterialNpcDiff].wallUs==5 && s.materials[MaterialNpcCleanup].wallUs==10,
            "exact lock/prepare/calc/diff/cleanup partition");
    require(s.materials[MaterialNpcCleanup].calls==2 && !s.materials[MaterialMapDiff].calls && s.materials[MaterialMapCleanup].wallUs==4,"optional diff and repeated cleanup segments are explicit");
    require(s.diffs[0]==2 && s.diffs[1]==7 && s.diffs[2]==4 && s.diffs[3]==4 && s.diffs[4]==0,"diff invocation/scanned/flagged/actual-call aggregates");
    WindowStart=Wall;Wall+=5000000000ll;InjectMaterialReportCommit=true;Report();bool total=false,cleanup=false,census=false;
    for(const auto& l:Logs){total|=l.find("workload-anim-material: npc-total calls=1 wall_samples=1 wall_us=21 cpu_samples=0")!=std::string::npos;cleanup|=l.find("workload-anim-material: npc-cleanup calls=2 wall_samples=2 wall_us=10 cpu_samples=0")!=std::string::npos;census|=l.find("npc-diff-census invocations=2 scanned=7 flagged=4 material_calls=4")!=std::string::npos;}
    require(total && cleanup && census && snapshot().families[FamilyNpcMaterial].calls==2 && AnimPrevious.families[FamilyNpcMaterial].calls==1,"material phases/census share category/family snapshot and baseline despite mid-report producer");
    Logs.clear();Wall+=5000000000ll;Report();bool carried=false;for(const auto& l:Logs)carried|=l.find("workload-anim-material: npc-diff calls=1 wall_samples=1 wall_us=2 cpu_samples=0")!=std::string::npos;
    require(carried,"next material report carries intervening producer exactly once");
}
static void materialNested() {
    beginTurn();const auto p=port_workload_anim_begin(),c=port_workload_anim_category_begin(0,6),f=port_workload_anim_family_begin(2);
    Wall+=1000;port_workload_anim_material_checkpoint(f,1);port_workload_anim_material_checkpoint(f,2);
    const auto nested=port_workload_anim_family_begin(2);const unsigned reads=WallReads;
    port_workload_anim_material_checkpoint(nested,99);port_workload_anim_material_checkpoint(nested,1);
    require(WallReads==reads,"known nested tokens ignore even wrong phase without reading clocks");
    Wall+=2000;port_workload_anim_family_end(nested);port_workload_anim_material_checkpoint(f,4);port_workload_anim_material_checkpoint(f,3);
    const auto nested2=port_workload_anim_family_begin(2);const auto ctx=port_workload_anim_material_diff_context();require(ctx==f,"nested diff census belongs to tracked outer diff");
    port_workload_anim_material_diff_counts(ctx,3,2,2);port_workload_anim_material_checkpoint(nested2,1);Wall+=3000;port_workload_anim_family_end(nested2);
    port_workload_anim_material_checkpoint(f,4);port_workload_anim_family_end(f);port_workload_anim_category_end(c);port_workload_anim_end(p);endTurn();const auto s=snapshot();checkMaterial(s);
    require(s.materials[MaterialNpcCalc].wallUs==2 && s.materials[MaterialNpcDiff].wallUs==3 && s.materialCensus[MaterialNestedMarks]==3,"nested material work stays only in outer owner's current phase");
    require(s.diffs[0]==1 && s.diffs[1]==3 && s.diffs[3]==2,"nested invocation submits its distinct loop work once");
}
static void materialMalformed() {
    uint32_t stale=0;
    for(unsigned scenario=0;scenario<7;++scenario) {
        beginTurn();const auto p=port_workload_anim_begin(),c=port_workload_anim_category_begin(0,6),f=port_workload_anim_family_begin(2);
        if(!scenario) {
            stale=f;const auto wall=WallReads,cpu=CpuReads;
#if defined(WORKLOAD_TEST_EMUTLS)
            const auto tls=TlsLookups.load();
#endif
            errno=E2BIG;port_workload_anim_material_checkpoint(0,99);port_workload_anim_material_diff_counts(0,99,99,99);
            require(WallReads==wall && CpuReads==cpu && errno==E2BIG,"enabled zero-token material marks cannot touch clocks/errno or close live owner");
#if defined(WORKLOAD_TEST_EMUTLS)
            require(TlsLookups.load()==tls,"enabled zero-token material marks do not look up observer TLS");
#endif
        }
        if(scenario==0) { std::thread foreign([&]{errno=E2BIG;port_workload_anim_material_checkpoint(f,1);require(!port_workload_anim_material_diff_context(),"foreign material census ignored");port_workload_anim_material_diff_counts(f,1,1,1);require(errno==E2BIG,"first-use foreign material TLS preserves errno");});foreign.join();port_workload_anim_material_checkpoint(f,2); }
        if(scenario==1)port_workload_anim_material_checkpoint(f+999,1);
        if(scenario==2){Wall-=1;port_workload_anim_material_checkpoint(f,1);}
        if(scenario==3){} // Missing checkpoints must not become cleanup residual.
        if(scenario==4)port_workload_anim_material_diff_counts(f,1,1,1);
        if(scenario==5){port_workload_anim_material_checkpoint(f,1);port_workload_anim_material_checkpoint(f,2);port_workload_anim_material_checkpoint(f,4);port_workload_anim_material_checkpoint(f,3);port_workload_anim_material_diff_counts(f,1,2,2);}
        if(scenario==6){port_workload_anim_material_checkpoint(f,1);port_workload_anim_material_checkpoint(f,2);port_workload_anim_material_checkpoint(f,4);port_workload_anim_material_checkpoint(f,3);port_workload_anim_material_checkpoint(f,4);port_workload_anim_material_checkpoint(f,3);}
        port_workload_anim_family_end(f);port_workload_anim_category_end(c);port_workload_anim_end(p);endTurn();
    }
    beginTurn();const auto p=port_workload_anim_begin(),c=port_workload_anim_category_begin(0,6),f=port_workload_anim_family_begin(2);
    port_workload_anim_material_checkpoint(stale,1);port_workload_anim_family_end(f);port_workload_anim_category_end(c);port_workload_anim_end(p);endTurn();
    const auto s=snapshot();checkMaterial(s);require(s.census[AnimInvalid]==8 && !s.families[FamilyNpcMaterial].calls,"bad/stale token, phase, clock, missing and census cases reject affected parents");
    require(s.materialCensus[MaterialBadToken]==2 && s.materialCensus[MaterialBadPhase]==3 && s.materialCensus[MaterialClockInvalid]==1 && s.materialCensus[MaterialUnfinished]==1 && s.materialCensus[MaterialCensusInvalid]==1,"material rejection reasons remain explicit");
}
static void materialRounding() {
    beginTurn();const auto p=port_workload_anim_begin(),c=port_workload_anim_category_begin(0,6);
    for(unsigned i=0;i<2;++i){const auto f=port_workload_anim_family_begin(2);for(unsigned phase:{1u,2u,4u,3u,4u}){Wall+=600;port_workload_anim_material_checkpoint(f,phase);}Wall+=600;port_workload_anim_family_end(f);}
    port_workload_anim_category_end(c);port_workload_anim_end(p);endTurn();const auto s=snapshot();checkMaterial(s);
    require(s.families[FamilyNpcMaterial].wallUs==7 && s.materials[MaterialNpcEntryLock].wallUs==1 && s.materials[MaterialNpcPrepare].wallUs==1 && s.materials[MaterialNpcCalc].wallUs==1 && s.materials[MaterialNpcDiff].wallUs==1 && s.materials[MaterialNpcCleanup].wallUs==3,"phase ns aggregate per parent and cleanup absorbs exact-us correction");
}
static void materialConcurrent() {
    beginTurn();endTurn();std::atomic<bool> done{false};std::atomic<unsigned> reads{0};
    std::thread reader([&]{do{AnimSnapshot s;if(ReadAnimSnapshot(s)){checkMaterial(s);const auto n=s.families[FamilyNpcMaterial].calls;require(s.materials[MaterialNpcEntryLock].wallUs==n && s.materials[MaterialNpcPrepare].wallUs==n*2 && s.materials[MaterialNpcCalc].wallUs==n*3 && s.materials[MaterialNpcCleanup].wallUs==n*4,"material payload and family total remain one concurrent snapshot");++reads;}}while(!done.load());});
    for(unsigned i=0;i<2000;++i){beginTurn();const auto p=port_workload_anim_begin(),c=port_workload_anim_category_begin(0,6),f=port_workload_anim_family_begin(2);Wall+=1000;port_workload_anim_material_checkpoint(f,1);Wall+=2000;port_workload_anim_material_checkpoint(f,2);Wall+=3000;port_workload_anim_material_checkpoint(f,4);Wall+=4000;port_workload_anim_family_end(f);port_workload_anim_category_end(c);port_workload_anim_end(p);endTurn();}
    done=true;reader.join();require(reads.load()!=0,"concurrent material snapshot reader actually ran");
}
static void materialSampling() {
    for(unsigned i=0;i<32;++i){beginTurn();const unsigned wall=WallReads,cpu=CpuReads;const auto p=port_workload_anim_begin(),c=port_workload_anim_category_begin(0,6),f=port_workload_anim_family_begin(2);require((f!=0)==(i%31==0),"material shares existing whole-turn selection");for(unsigned phase:{1u,2u,4u}){Wall+=1000;port_workload_anim_material_checkpoint(f,phase);}Wall+=1000;port_workload_anim_family_end(f);port_workload_anim_category_end(c);port_workload_anim_end(p);require(CpuReads==cpu && WallReads-wall==(f?9u:0u),"unselected material marks never read clocks");endTurn();}
    const auto s=snapshot();checkMaterial(s);require(s.families[FamilyNpcMaterial].calls==2 && s.materials[MaterialNpcCalc].wallUs==2,"stride31 material selection is not independently resampled");
}
static void materialDiscard() {
    beginTurn();const auto p=port_workload_anim_begin();auto c=port_workload_anim_category_begin(0,6),f=port_workload_anim_family_begin(2);
    for(unsigned phase:{1u,2u,4u,3u}){Wall+=1000;port_workload_anim_material_checkpoint(f,phase);}port_workload_anim_material_diff_counts(port_workload_anim_material_diff_context(),9,4,4);Wall+=1000;port_workload_anim_material_checkpoint(f,4);Wall+=1000;port_workload_anim_family_end(f);port_workload_anim_category_end(c);
    c=port_workload_anim_category_begin(0,5);f=port_workload_anim_family_begin(2);port_workload_anim_family_end(f);port_workload_anim_category_end(c);port_workload_anim_end(p);endTurn();const auto s=snapshot();checkMaterial(s);
    require(!s.families[FamilyNpcMaterial].calls && !s.materials[MaterialNpcCalc].calls && !s.diffs[0] && !s.diffs[1],"later sibling invalidation discards earlier completed phases and aggregate diff census");
}
#endif
static void materialScenario(const std::string& mode) {
#if defined(VGX_ANIM_MATERIAL_VERSION)
    if(mode=="material-timing")materialTiming();else if(mode=="material-nested")materialNested();else if(mode=="material-malformed")materialMalformed();else if(mode=="material-rounding")materialRounding();else if(mode=="material-concurrent")materialConcurrent();else if(mode=="material-sampling")materialSampling();else if(mode=="material-discard")materialDiscard();else materialDisabled();
#else
    (void)mode;beginTurn();(void)port_workload_anim_begin();(void)port_workload_anim_category_begin(0,6);const auto f=port_workload_anim_family_begin(2);
    for(unsigned phase:{1u,2u,4u,3u})port_workload_anim_material_checkpoint(f,phase);
    require(port_workload_anim_material_diff_context()!=0,"tracked material diff must expose owner context");
#endif
}
#endif

int main(int argc,char** argv) {
    require(argc==2,"scenario argument"); const std::string mode=argv[1];
#if defined(__vita__) && PORT_LOGGING
#if defined(VGX_ANIM_CATEGORIES_VERSION)
    if(mode.find("material-")==0)materialScenario(mode);
    else if(mode.find("family-")==0) familyScenario(mode);
    else if(mode=="timing") timing(); else if(mode=="malformed") malformed(); else if(mode=="sampling") sampling(); else if(mode=="overflow") overflow();
    else if(mode=="labels") labels(); else if(mode=="classification") classification(); else if(mode=="concurrent") concurrent();
    else {
        const unsigned wall=WallReads,cpu=CpuReads;
#if defined(WORKLOAD_TEST_EMUTLS)
        const unsigned tls=TlsLookups;
#endif
        ClobberErrno=true; errno=E2BIG;
        require(port_workload_anim_begin()==0 && port_workload_anim_category_begin(0,4)==0,"disabled category observer returns inactive tickets");
        port_workload_anim_end(123); port_workload_anim_category_end(123); port_workload_anim_end(0); port_workload_anim_category_end(0);
        require(WallReads==wall && CpuReads==cpu && errno==E2BIG,"disabled observer has no clocks or errno effects");
#if defined(WORKLOAD_TEST_EMUTLS)
        require(TlsLookups==tls,"disabled observer never looks up emulated TLS");
#endif
    }
#else
    if(mode=="timing") { port_perf_frame_begin(); require(port_workload_anim_begin()!=0,"selected context must receive an active ticket"); }
#endif
#else
    (void)mode;
#endif
    std::printf("PASS: category observer %s\n",mode.c_str());
}
