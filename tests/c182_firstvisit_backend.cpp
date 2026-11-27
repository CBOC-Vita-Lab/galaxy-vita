// Test the production queue and registration paths, without creating a GL context or drawing anything.
// (seedrun31) Also: the priority from the record's tag alone (no built-in hashes), the scenario order, the can-draw tier kept apart, the labels of new records, and the
// stage journal (a session killed in play keeps its records: written by the writer thread, read back at the next boot, emptied once the stage file has them).
// Linux/EGL host: CPATH=<Khronos GLES2 headers> CXX=clang++ bash tools/test_c182_firstvisit_backend.sh
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#include <cassert>
#include <csignal>
#include <set>
#include <sys/wait.h>
#include <unistd.h>
static uint32_t g_testScene = 0x17bab031u, g_testLoadingKey = 0;
static const char* g_testLoadingName = nullptr;
static int g_testScenario = -1;
extern "C" uint32_t port_scene_key() { return g_testScene; }
extern "C" int port_scene_loading_stage(char* name, int cap, int* scenario) {
    if (!g_testLoadingName) return 0;
    std::snprintf(name, static_cast<size_t>(cap), "%s", g_testLoadingName);
    *scenario = g_testScenario;
    return 1;
}
extern "C" void port_log(const char*, ...) {}
static std::vector<std::string> g_testLines;
extern "C" void port_log_summary(const char* fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    g_testLines.emplace_back(text);
}
static bool Logged(const char* part) { for (const auto& l : g_testLines) if (l.find(part) != std::string::npos) return true; return false; }
namespace vgx { bool LogEnabled() { return false; } }
static uint32_t Fnv(const char* s) { uint32_t h = 2166136261u; for (; *s; ++s) h = (h ^ static_cast<unsigned char>(*s)) * 16777619u; return h; }
static void SeedrunCases();
static void JournalCases(const std::string& dir);
static void RealListCases(const std::string& dir);
int main() {
    using namespace vgx;
    using namespace vgx::backend;
    char dirTemplate[] = "/tmp/fvbXXXXXX";
    const std::string dir = mkdtemp(dirTemplate);
    setenv("GXV_STAGEWARM_FILE", (dir + "/vgx_stagewarm.bin").c_str(), 1); // (seedrun31: the journal's path; nothing is read or written before JournalCases)
    setenv("GXV_STAGEWARM_MENU", "0", 1);
    setenv("GXV_STAGEWARM_COMMON", "0", 1);
    setenv("GXV_BE_FAST2", "0", 1);
    g_stageLoaded = true;
    g_beFast2 = false;
    constexpr uint32_t stage = 0x17bab031u;
    ShaderSource v{}, f{}, heavyV{}, heavyF{}, late{}, unrelated{};
    v.id = 1; v.keyHash = 10; f.fragment = true; f.id = 2; f.keyHash = 20;
    heavyV.id = 3; heavyV.keyHash = 0xefb2f05617d51094ull;
    heavyF.fragment = true; heavyF.id = 4; heavyF.keyHash = 0x6652cbb3c18517deull;
    late.id = 5; late.keyHash = 500; unrelated.id = 6; unrelated.keyHash = 600;
    RegisterShader(&v); RegisterShader(&f); RegisterShader(&heavyV); RegisterShader(&heavyF);
    StageAdd(stage, {10, 20, 0xc00});
    StageAdd(stage, {10, 20, 0x2c00});
    StageAdd(stage, {heavyV.keyHash, heavyF.keyHash, 0x1016400 | firstvisit::slowTag}); // (seedrun31: the priority is the tag in the data, no built-in hash)
    StageAdd(kPlayerKey, {10, 20, 0xc00});
    StageAdd(kPlayerKey, {late.keyHash, 20, 0x2c00});
    StageBuild(stage);
    assert(g_stageQueue.size() == 3 && g_stageQueue.front().vsId == 3 && g_firstVisit.Waiting() == 1);
    g_stageQueue.pop_front(); g_stageQueue.pop_front();
    g_stageCompileMs = 123; g_stageCompileBudget = 999;
    RegisterShader(&unrelated);
    g_screenLive = false; StageFrameBegin();
    assert(g_stageQueue.size() == 1);
    RegisterShader(&late);
    g_screenLive = true; StageFrameBegin();
    assert(g_stageQueue.size() == 1 && g_firstVisit.HasReady());
    g_screenLive = false; StageFrameBegin();
    assert(g_stageQueue.size() == 2 && g_stageQueue.front().vsId == 5 && g_stageQueued == 4);
    assert(g_stageCompileMs == 123 && g_stageCompileBudget == 999);
    assert(g_stageProgs.count((5u << 16) | 2u) && !g_firstVisit.HasReady());
    const uint32_t key = (1u << 16) | 2u;
    g_screenLive = true; g_menuRec = false;
    g_programs[key].recStage = stage; g_programs[key].recLB = 0x2c00;
    StageDemandNote(1, 2, key, 1, 95); StageFlushPending();
    assert(g_stageSets.at(stage).slowCount == 1); // (the tagged record of the list)
    StageDemandNote(1, 2, key, 1, 203); StageFlushPending();
    assert(g_stageSets.at(stage).slowCount == 2 && g_stageSets.at(stage).recs.back().lb == (0x2c00u | firstvisit::slowTag));
    StageBuild(stage);
    assert(g_stageQueue.size() == 4 && g_stageFirstIds.count(key));
    assert(g_stageQueue.front().vsId == 1 && g_stageQueue.front().layoutMask == 1); // only the captured variant outranks the original order
    std::puts("production stage paths: exact dedup, cold-priority, saved progress, no live insertion, late player and learned slow-link PASS");
    SeedrunCases();
    JournalCases(dir);
    RealListCases(dir);
    std::string rm = "rm -rf " + dir;
    if (std::system(rm.c_str()) != 0) return 1;
}
namespace {
using namespace vgx;
using namespace vgx::backend;
std::vector<std::unique_ptr<ShaderSource>> g_testSrc;
void Reg(uint64_t hash, bool fragment) {
    auto s = std::make_unique<ShaderSource>();
    s->fragment = fragment;
    s->keyHash = hash;
    s->id = static_cast<uint16_t>(fragment ? g_fsSrc.size() : g_vsSrc.size());
    RegisterShader(s.get());
    g_testSrc.push_back(std::move(s));
}
uint32_t IdOfVs(uint64_t h) { for (size_t i = 0; i < g_vsSrc.size(); ++i) if (g_vsSrc[i] && g_vsSrc[i]->keyHash == h) return static_cast<uint32_t>(i); return ~0u; }
std::vector<uint64_t> QueueVs() { std::vector<uint64_t> out; for (const VariantCmd& v : g_stageQueue) out.push_back(g_vsSrc[v.vsId]->keyHash); return out; }
}
// (seedrun31 A3 / A4) the scenario order, the tag rule, the can-draw tier, the labels
static void SeedrunCases() {
    using firstvisit::ScenarioBits;
    using firstvisit::slowTag;
    using firstvisit::canDrawTag;
    const uint32_t s2 = Fnv("SeedrunTestGalaxy");
    for (uint64_t h = 101; h <= 110; ++h) { Reg(h, false); Reg(h + 100, true); }
    g_stageSets.erase(s2);
    // A any, B scenario 2, C scenario 3, D scenario 3 slow, E scenario 2 slow, F any slow (in the list before the priority), and the player list's P
    StageAdd(s2, {101, 201, 0x400});
    StageAdd(s2, {102, 202, 0x400 | ScenarioBits(2)});
    StageAdd(s2, {103, 203, 0x400 | ScenarioBits(3)});
    StageAdd(s2, {104, 204, 0x800 | slowTag | ScenarioBits(3)});
    StageAdd(s2, {105, 205, 0x800 | slowTag | ScenarioBits(2)});
    g_stageSets[kPlayerKey].recs.clear();
    StageAdd(kPlayerKey, {106, 206, 0x400});
    // the can-draw tier: X new, a duplicate of A (queued already), Y with a shader this run does not know; and from the package, Z
    assert(!CanDrawAdd(s2, {101, 201, 0x400}, false));                     // (not a can-draw record)
    assert(CanDrawAdd(s2, {107, 207, 0xc00 | canDrawTag}, false));
    assert(!CanDrawAdd(s2, {107, 207, 0xc00 | canDrawTag}, true));         // (a duplicate)
    assert(CanDrawAdd(s2, {101, 201, 0x400 | canDrawTag}, true));
    assert(CanDrawAdd(s2, {999, 207, 0x400 | canDrawTag}, true));
    assert(CanDrawAdd(s2, {108, 208, 0x400 | canDrawTag | ScenarioBits(2)}, true));
    g_testLoadingName = "SeedrunTestGalaxy";
    const auto build = [&](int sc) {
        g_testScenario = sc;
        g_stageTarget = 0;
        StageBuild(s2);
        const bool mine = g_testLoadingName && std::strcmp(g_testLoadingName, "SeedrunTestGalaxy") == 0;
        assert(g_stageTargetScenario == static_cast<unsigned>(ScenarioOn() && mine && sc >= 1 && sc <= 7 ? sc : 0));
        return QueueVs();
    };
    if (!ScenarioOn()) {
        // (fix31) the default, GXV_STAGEQ_SCENARIO=0 (owner 17 Nov: off until the console A/B): no hint is read: every scenario gets the record order, exactly as
        // before seedrun31 (the slow tags of every scenario first), and the stage being played has no scenario, so a new record is never hinted
        const std::vector<uint64_t> before{104, 105, 101, 102, 103, 106};
        assert(build(3) == before && build(2) == before && build(0) == before && build(9) == before);
        assert(StageScenario(s2) == 0);
        g_testLoadingName = nullptr;
        g_stageSets.erase(s2);
        g_canDraw.erase(s2);
        std::puts("fix31: GXV_STAGEQ_SCENARIO=0 (default): the record order of every scenario is the order before seedrun31, no hint read PASS");
        return;
    }
    // scenario 3: D (slow, this scenario) first; then A, C (this scenario or any, record order), the player list, then the other scenario's B, E (E's tag is not this scenario's)
    assert(build(3) == (std::vector<uint64_t>{104, 101, 103, 106, 102, 105}));
    assert(g_stageFirstIds.size() == 1 && g_stageOwnOpen.size() == 6 && g_stageLeft.load() == 6);
    // the tier: X, then Z (of scenario 2: behind), not the duplicate of A, not Y; none of it in the queue, its count, the hold or the spared programs
    assert(g_stageCanDraw.size() == 2 && IdOfVs(107) == g_stageCanDraw[0].vsId && IdOfVs(108) == g_stageCanDraw[1].vsId);
    assert(!g_stageProgs.count((IdOfVs(107) << 16) | g_stageCanDraw[0].fsId) && g_stageQueued == 6);
    assert(Logged("can-draw tier: 2 draw variants behind the queue") && Logged("1 records with shaders this run does not know, 1 queued already"));
    // scenario 2: E first; A, B, the player list, C, D
    assert(build(2) == (std::vector<uint64_t>{105, 101, 102, 106, 103, 104}));
    assert(g_stageCanDraw.size() == 2 && IdOfVs(107) == g_stageCanDraw[0].vsId && IdOfVs(108) == g_stageCanDraw[1].vsId); // (Z is this scenario's now: in record order)
    // not known (another stage's load, a scenario above 7): the record order, as before seedrun31; the slow tags of every scenario count
    assert(build(0) == (std::vector<uint64_t>{104, 105, 101, 102, 103, 106}));
    assert(build(9) == (std::vector<uint64_t>{104, 105, 101, 102, 103, 106}));
    g_testLoadingName = "AnotherGalaxy";
    assert(build(3) == (std::vector<uint64_t>{104, 105, 101, 102, 103, 106}));
    g_testLoadingName = "SeedrunTestGalaxy";
    // the tier waits while the queue has entries, in play, and when nothing covers the screen
    build(3);
    g_screenLive = false;
    assert(!RunCanDraw());
    g_stageQueue.clear();
    g_screenLive = true;
    assert(!RunCanDraw());
    g_screenLive = false;
    g_stageHoldStampMs.store(0.0);
    assert(!RunCanDraw() && g_stageCanDraw.size() == 2);
    // the labels of new records (the stage being played is in scenario 3)
    StageRec r{101, 201, 0x400};
    assert(!StageLabel(s2, 3, r));                                          // (A is held for any scenario)
    r = {103, 203, 0x400};
    assert(!StageLabel(s2, 3, r));                                          // (C is held for this scenario)
    r = {102, 202, 0x400};
    assert(StageLabel(s2, 3, r) && r.lb == 0x400);                          // (B was scenario 2's: seen in a second scenario, any)
    r = {109, 209, 0x400};
    assert(StageLabel(s2, 3, r) && r.lb == (0x400 | ScenarioBits(3)) && StageAdd(s2, r));
    r = {109, 209, 0x400};
    assert(!StageLabel(s2, 3, r));
    r = {109, 209, 0x400};
    assert(StageLabel(s2, 5, r) && r.lb == 0x400 && StageAdd(s2, r));       // (and in scenario 5: any)
    r = {109, 209, 0x400};
    assert(!StageLabel(s2, 6, r));
    r = {110, 210, 0x400};
    assert(StageLabel(s2, 0, r) && r.lb == 0x400);                          // (not known: unhinted, as before)
    g_testLoadingName = nullptr;
    g_stageSets.erase(s2);
    g_canDraw.erase(s2);
    std::puts("seedrun31: tag-only priority, scenario order (others behind the player list), can-draw tier apart from the queue and the hold, record labels PASS");
}
// (seedrun31 A3) the stage journal: a child learns records, its writer thread writes the journal, the child is killed (a session closed in play); the next "boot" reads
// them back, the quiet-moment append writes them to the stage file, and the journal is written again empty.
static void JournalCases(const std::string& dir) {
    const std::string stageFile = dir + "/vgx_stagewarm.bin", jnl = stageFile + ".jnl";
    assert(StageFile() && stageFile == StageFile() && StageJournalPath() == jnl);
    const uint32_t s3 = 0x5eed0003u;
    const auto boot = [&] { // (what a new process has: nothing in memory, the files on the card)
        g_stageSets.clear(); g_canDraw.clear(); g_stageRecs = 0; g_canDrawRecs = 0; g_stageFileRecs = 0; g_stageLoaded = false; g_jnlOnDisk = 0; g_jnlDirty = false;
        g_coverNewRecs = 0; g_coverNewCd = 0;
        StageLoad();
        assert(g_coverNewRecs == 0 && g_coverNewCd == 0 && !g_stageLoading); // (fix31: what the start-up loads read is not learned: the cover: line's counts stay 0)
    };
    boot(); // (an empty card: the stage file is started)
    const pid_t pid = fork();
    if (pid == 0) {
        StageAdd(s3, {1001, 2001, 0x400});
        StageAdd(s3, {1002, 2002, 0x800 | firstvisit::slowTag});
        CanDrawAdd(s3, {1003, 2003, 0xc00 | firstvisit::canDrawTag}, false);
        StageJournalMark();
        g_jnlDirtyMs -= 10000.0;
        StageJournalPoll(NowMs());
        for (int i = 0; i < 400 && !g_jnlDone.load(); ++i) usleep(5000);
        raise(SIGKILL); // (no flush, no exit handlers: as a session the PS button or a crash ends)
    }
    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    std::vector<StageFileRec> got;
    assert(PlyRead(jnl, got, kJnlMagic, kJnlMaxRecs) && got.size() == 3);
    boot();
    assert(g_stageSets.count(s3) && g_stageSets[s3].recs.size() == 2 && g_stageSets[s3].persisted == 0 && g_stageSets[s3].recs[1].lb == (0x800 | firstvisit::slowTag));
    assert(g_canDraw.count(s3) && g_canDraw[s3].recs.size() == 1 && g_canDraw[s3].persisted == 0);
    assert(Logged("journal: 3 records read from") && g_jnlOnDisk == 3);
    StageFlush(); // (a quiet moment: the stage file gets them; the journal is due again)
    assert(g_stageSets[s3].persisted == 2 && g_canDraw[s3].persisted == 1 && g_jnlDirty);
    g_jnlDirtyMs -= 10000.0;
    g_jnlLastHandMs = -1e9;
    const unsigned done0 = g_jnlDone.load();
    StageJournalPoll(NowMs());
    for (int i = 0; i < 400 && g_jnlDone.load() == done0; ++i) usleep(5000);
    assert(PlyRead(jnl, got, kJnlMagic, kJnlMaxRecs) && got.empty());
    boot(); // (the stage file alone holds them, once each)
    assert(g_stageSets[s3].recs.size() == 2 && g_canDraw[s3].recs.size() == 1 && g_stageSets[s3].persisted == 2);
    // the package's can-draw records: kept apart, never written to the card
    const std::string pkg = dir + "/pkg.bin";
    FILE* f = std::fopen(pkg.c_str(), "wb");
    std::fwrite(kStageMagic, 1, 8, f);
    const StageFileRec pr[2] = {{s3, 0x400 | firstvisit::canDrawTag, 1004, 2004}, {s3, 0x400 | firstvisit::slowTag, 1005, 2005}};
    std::fwrite(pr, sizeof pr, 1, f);
    std::fclose(f);
    setenv("GXV_STAGEWARM_PKG_FILE", pkg.c_str(), 1);
    boot();
    assert(g_canDraw[s3].pkg.size() == 1 && g_canDraw[s3].recs.size() == 1 && g_stageSets[s3].recs.size() == 3);
    assert(Logged("1 slow, 0 with a scenario hint, 1 can-draw records"));
    assert(Logged("has no stagetags stamp")); // (seedrun31b: an untagged package list is said loudly)
    StageFlush();
    unsetenv("GXV_STAGEWARM_PKG_FILE");
    boot();
    assert(g_canDraw[s3].recs.size() == 1 && g_canDraw[s3].pkg.empty() && g_stageSets[s3].recs.size() == 3); // (the package's own record joined the card, its can-draw one did not)
    // (fix31) the cover: line: records learned in the visit (own and can-draw apart), visit totals kept across a periodic line, cleared after the visit's last line;
    // a visit that paid nothing in play but learned records still has its line
    StageAdd(s3, {1006, 2006, 0x400});
    StageAdd(s3, {1006, 2006, 0x400}); // (a duplicate: not learned)
    CanDrawAdd(s3, {1007, 2007, 0xc00 | firstvisit::canDrawTag}, false);
    CanDrawAdd(s3, {1008, 2008, 0xc00 | firstvisit::canDrawTag}, true); // (the package's: not learned)
    assert(g_coverNewRecs == 1 && g_coverNewCd == 1);
    g_fu = FirstUseLedger{}; g_fuStage = s3; g_testLines.clear();
    FirstUseSummary("periodic");
    assert(Logged("cover: stage=5eed0003 why=periodic") && Logged("new_recs=1 new_candraw=1 ") && !Logged("first-use:") && g_coverNewRecs == 1 && g_coverNewCd == 1);
    StageAdd(s3, {1009, 2009, 0x400});
    g_testLines.clear();
    FirstUseSummary("left");
    assert(Logged("cover: stage=5eed0003 why=left") && Logged("new_recs=2 new_candraw=1 ") && g_coverNewRecs == 0 && g_coverNewCd == 0);
    g_testLines.clear();
    FirstUseSummary("left");
    assert(g_testLines.empty()); // (nothing paid, nothing learned: no line, as before)
    std::puts("seedrun31: stage journal (writer thread, SIGKILL in play, read back at boot, emptied after the append), package can-draw records never on the card PASS");
    std::puts("fix31: cover: new_recs / new_candraw count what the visit learned, never the start-up loads; visit totals; a learning visit has its line PASS");
}

// (seedrun31b) The real packaged list, through the production loader and queue build: FV_REAL_TAGGED = a list made by tools/seedrun/stagetags.py legacy (the test
// script passes test19's when it is given the VPK). An empty card; every key hash of the list registered; AstroGalaxy and HoneyBeeKingdomGalaxy built: their
// historical records (all unlinked) are the head, in record order, and the old player seed's records are the last of the player tier, as the built-in seed was.
static void RealListCases(const std::string& dir) {
    const char* tagged = std::getenv("FV_REAL_TAGGED");
    if (!tagged || !*tagged) { std::puts("seedrun31b: real list: FV_REAL_TAGGED not set, skipped"); return; }
    setenv("GXV_STAGEWARM_PKG_FILE", tagged, 1);
    std::remove((dir + "/vgx_stagewarm.bin").c_str());
    std::remove((dir + "/vgx_stagewarm.bin.jnl").c_str());
    std::remove((dir + "/vgx_stagewarm.bin.ply").c_str());
    g_stageSets.clear(); g_canDraw.clear(); g_stageRecs = 0; g_canDrawRecs = 0; g_stageFileRecs = 0; g_stageLoaded = false; g_jnlOnDisk = 0; g_jnlDirty = false;
    g_testLines.clear();
    StageLoad();
    unsetenv("GXV_STAGEWARM_PKG_FILE");
    assert(!Logged("has no stagetags stamp") && Logged("tags (seedrun31): 9 slow"));
    std::set<uint64_t> vs, fs;
    for (const auto& kv : g_stageSets) for (const StageRec& r : kv.second.recs) { vs.insert(r.vs); fs.insert(r.fs); }
    for (uint64_t h : vs) if (IdOfVs(h) == ~0u) Reg(h, false);
    for (uint64_t h : fs) { bool have = false; for (const ShaderSource* x : g_fsSrc) if (x && x->keyHash == h) { have = true; break; } if (!have) Reg(h, true); }
    g_programs.clear();
    const struct { uint32_t stage; std::vector<std::pair<uint64_t, uint64_t>> head; } cases[] = {
        {0x4903d024u, {{0x671b7a9fae441691ull, 0x0d14f45bae134e25ull}, {0x1c119354708911ffull, 0x854159d5c0a23060ull}, {0x671b7a9fae441691ull, 0x85da8833ca90cc24ull},
                       {0x1c119354708911ffull, 0xb0119fce9ea638a1ull}, {0x7ed555ea91ecd182ull, 0x8ec5794f6b01cbafull}, {0x1fb3fa05b81b6e91ull, 0xec49bbef9d0c8383ull}}},
        {0x17bab031u, {{0x166a6a6b5c0367f7ull, 0x11c015bfcb16a96cull}, {0xefb2f05617d51094ull, 0x6652cbb3c18517deull}, {0xe94d45d78111dceeull, 0x6652cbb3c18517deull}}},
    };
    const auto& ply = g_stageSets.at(kPlayerKey).recs;
    assert(ply.size() >= 9);
    for (const auto& c : cases) {
        g_stageTarget = 0;
        StageBuild(c.stage);
        for (size_t i = 0; i < c.head.size(); ++i)
            assert(g_vsSrc[g_stageQueue[i].vsId]->keyHash == c.head[i].first && g_fsSrc[g_stageQueue[i].fsId]->keyHash == c.head[i].second);
        assert(g_stageFirstIds.size() == c.head.size());
        // the last player records of the list (the old seed's, added by stagetags) are queued as the last entries of the head (own, then player: nothing is linked)
        const size_t seg = g_stageOwnOpen.size();
        const StageRec& last = ply.back();
        bool found = false;
        for (size_t i = 0; i < g_stageQueue.size() && !found; ++i)
            found = g_vsSrc[g_stageQueue[i].vsId]->keyHash == last.vs && g_fsSrc[g_stageQueue[i].fsId]->keyHash == last.fs;
        assert(found && seg > 0);
        std::printf("seedrun31b: real list: stage %08x: %zu queued, the %zu historical records are entries 0..%zu (production StageBuild from the packaged list alone)\n",
                    c.stage, g_stageQueue.size(), c.head.size(), c.head.size() - 1);
    }
    std::puts("seedrun31b: real tagged list through the production loader and StageBuild PASS");
}
