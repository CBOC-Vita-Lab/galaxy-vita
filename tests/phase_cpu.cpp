#include <atomic>
#include <cerrno>
#include <cfenv>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <time.h>
#include "../platform/vgx/src/vgx_common.h"

struct FakeClock {
    int64_t wall = 1000000000, cpu = 1000000;
    unsigned wallReads = 0, cpuReads = 0;
    bool failCpu = false, changeErrno = false;
};
static thread_local FakeClock clockState;
static std::atomic<unsigned> parses{0};
static bool insideCheckpoint = false;
namespace vgx {
static long PhaseEnvInt(const char* name, long fallback) {
    if (strcmp(name, "GXV_PHASECPU") == 0) ++parses;
    return EnvInt(name, fallback);
}
}
static int PhaseClockGettime(clockid_t id, timespec* ts) {
    if (id != CLOCK_THREAD_CPUTIME_ID) std::abort();
    ++clockState.cpuReads;
    if (clockState.changeErrno) errno = EDOM;
    if (clockState.failCpu) return -1;
    ts->tv_sec = clockState.cpu / 1000000;
    ts->tv_nsec = (clockState.cpu % 1000000) * 1000;
    return 0;
}
#define clock_gettime PhaseClockGettime
#define EnvInt PhaseEnvInt
#include "../platform/src/port/perf.cpp"
#undef EnvInt
#undef clock_gettime

static std::vector<std::string> logs;
static std::mutex logsLock;
extern "C" int64_t port_host_time_ns() {
    ++clockState.wallReads;
    if (clockState.changeErrno) errno = ERANGE;
    return clockState.wall;
}
#if defined(__vita__)
extern "C" int64_t port_thread_cpu_us() { return clockState.cpu; }
#endif
extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char* fmt, ...) {
    if (insideCheckpoint) std::abort();
#if PORT_PHASE_CPU_COMPILED
    const unsigned wallReads=clockState.wallReads,cpuReads=clockState.cpuReads;
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_DRAW);
    if (clockState.wallReads!=wallReads || clockState.cpuReads!=cpuReads) std::abort();
#endif
    char line[2048]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    std::lock_guard<std::mutex> lock(logsLock);
    logs.emplace_back(line);
    errno = ENOSPC;
}
extern "C" int port_handheld() { return 0; }
extern "C" unsigned port_vgx_backend_pending() { return 0; }
extern "C" int64_t port_vgx_player_cpu_us() { return 0; }
extern "C" void port_vgx_diagnostic_csv_pause(int) {}
extern "C" void port_autoq_game_begin() {}
extern "C" void port_autoq_game_work_done() {}
extern "C" void port_settings_panel_turn(int) {}
namespace MR { const char* getCurrentStageName() { return ""; } }

static void require(bool yes, const char* why) {
    if (!yes) { fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static void advance(int64_t wall, int64_t cpu) { clockState.wall += wall; clockState.cpu += cpu; }
static void checkpoint(uint32_t phase) {
    insideCheckpoint = true;
    PORT_GAME_PROGRESS(phase);
    insideCheckpoint = false;
}
static void turn() {
    checkpoint(PORT_GAME_PHASE_FRAME_BEGIN);
    port_perf_frame_begin();
    advance(10000, 2); checkpoint(PORT_GAME_PHASE_DRAW);
    advance(20000, 3); checkpoint(PORT_GAME_PHASE_CALC_ANIM);
    advance(30000, 5); port_perf_frame_work_done();
    checkpoint(PORT_GAME_PHASE_WAIT_RETRACE);
    advance(500000, 7);
    checkpoint(PORT_GAME_PHASE_FRAME_COMPLETE);
}
using Fields = std::map<std::string, std::string>;
static Fields fields(const std::string& line) {
    Fields result;
    size_t pos = 0;
    while (pos < line.size()) {
        const size_t end = line.find(' ', pos), eq = line.find('=', pos);
        if (eq != std::string::npos && (end == std::string::npos || eq < end))
            result[line.substr(pos, eq-pos)] = line.substr(eq+1, end == std::string::npos ? end : end-eq-1);
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return result;
}
static std::vector<Fields> rows() {
    std::vector<Fields> result;
    for (const auto& line : logs) if (line.find("phase-cpu cumulative ") == 0) result.push_back(fields(line));
    return result;
}
static uint64_t number(const Fields& row, const char* key) {
    auto it = row.find(key); require(it != row.end(), key);
    return std::stoull(it->second);
}
static Fields row(const char* phase, unsigned thread = 1, unsigned occurrence = 0) {
    for (const auto& r : rows()) {
        auto p = r.find("phase");
        if (p != r.end() && p->second == phase && number(r, "thread") == thread) {
            if (occurrence-- == 0) return r;
        }
    }
    require(false, "expected phase report exists"); return {};
}
static Fields header(unsigned thread = 1, unsigned occurrence = 0) {
    for (const auto& r : rows()) if (!r.count("phase") && number(r, "thread") == thread) {
        if (occurrence-- == 0) return r;
    }
    require(false, "expected cumulative header exists"); return {};
}
static void checkRow(const Fields& r, uint64_t intervals, uint64_t wallSamples, uint64_t wall,
                     uint64_t matched, uint64_t matchedWall, uint64_t cpu,
                     uint64_t badWall = 0, uint64_t badCpu = 0) {
    require(number(r,"intervals")==intervals && number(r,"wall-samples")==wallSamples && number(r,"wall-ns")==wall &&
            number(r,"matched")==matched && number(r,"matched-wall-ns")==matchedWall && number(r,"cpu-us")==cpu &&
            number(r,"invalid-wall")==badWall && number(r,"invalid-cpu")==badCpu, "literal interval totals match");
}

int main(int argc, char** argv) {
    require(argc == 4, "setting, enabled expectation, scenario required");
    if (strcmp(argv[1], "unset") == 0) unsetenv("GXV_PHASECPU"); else setenv("GXV_PHASECPU", argv[1], 1);
    const bool on = atoi(argv[2]) != 0;
    const std::string scenario = argv[3];
    if (scenario == "basic") {
        errno = E2BIG;
        turn();
        require(errno == E2BIG, "environment lookup preserves incoming errno");
        setenv("GXV_PHASECPU", on ? "0" : "1", 1);
        for (unsigned i=1; i<255; ++i) turn();
        require(rows().empty(), "no early report before completed turn 256");
        errno = E2BIG; turn();
        require(errno == E2BIG, "reporting preserves incoming errno");
        if (on) {
            require(rows().size()==5, "header, whole and three used phases reported");
            require(number(header(),"turns")==256 && number(header(),"unknown")==0, "completed denominator and initial unclassified seed");
            checkRow(row("whole"),256,256,15360000,256,15360000,2560);
            checkRow(row("unclassified"),256,256,2560000,256,2560000,512);
            checkRow(row("draw"),256,256,5120000,256,5120000,768);
            checkRow(row("calc-anim"),256,256,7680000,256,7680000,1280);
            require(sGameProgress.load()==0 && sTurn.phase==0, "native checkpoint leaves old global progress lane untouched");
            for (unsigned i=0;i<256;++i) turn();
            require(rows().size()==10 && number(header(1,1),"turns")==512, "second bounded report is cumulative");
            checkRow(row("whole",1,1),512,512,30720000,512,30720000,5120);
        } else require(rows().empty(), "disabled or compiled-out observer never reports");
#if !defined(__vita__) && !defined(PORT_ARMHF_HARNESS) && PORT_RUNTIME_DIAGNOSTICS && PORT_LOGGING
        require(parses.load()==1, "read-once real EnvInt parsing, including disabled setting");
#else
        require(parses.load()==0, "compiled-out observer does not inspect control");
#endif
    } else if (scenario == "faults") {
        for (unsigned i=0;i<256;++i) {
            clockState.wall=1000000000; clockState.cpu=1000000;
            port_perf_frame_begin();
            clockState.wall=0; clockState.failCpu=true; checkpoint(PORT_GAME_PHASE_DRAW);
            clockState.wall=1000000020; clockState.cpu=1000002; clockState.failCpu=false; checkpoint(PORT_GAME_PHASE_CALC_ANIM);
            clockState.wall=1000000010; clockState.cpu=1000001; checkpoint(PORT_GAME_PHASE_CAPTURE);
            clockState.wall=1000000040; clockState.cpu=1000004; port_perf_frame_work_done();
        }
        checkRow(row("whole"),256,256,10240,256,10240,1024);
        checkRow(row("unclassified"),256,0,0,0,0,0,256,256);
        checkRow(row("draw"),256,0,0,0,0,0,256,256);
        checkRow(row("calc-anim"),256,0,0,0,0,0,256,256);
        checkRow(row("capture"),256,256,7680,256,7680,768);
        const auto h=header();
        require(number(h,"wall-faults")==256 && number(h,"cpu-faults")==256 &&
                number(h,"wall-backwards")==256 && number(h,"cpu-backwards")==256, "read faults and backwards boundaries counted once");
    } else if (scenario == "matched") {
        for (unsigned i=0;i<256;++i) {
            port_perf_frame_begin();
            advance(10000,2); clockState.failCpu=true; checkpoint(PORT_GAME_PHASE_DRAW);
            advance(20000,3); clockState.failCpu=false; checkpoint(PORT_GAME_PHASE_CALC_ANIM);
            advance(30000,5); port_perf_frame_work_done();
        }
        checkRow(row("unclassified"),256,256,2560000,0,0,0,0,256);
        checkRow(row("draw"),256,256,5120000,0,0,0,0,256);
        checkRow(row("calc-anim"),256,256,7680000,256,7680000,1280);
        checkRow(row("whole"),256,256,15360000,256,15360000,2560);
    } else if (scenario == "endpoints") {
        for (unsigned i=0;i<256;++i) {
            clockState.wall=1000000000; clockState.cpu=1000000; clockState.failCpu=false;
            if (i%4==0) { clockState.wall=0; clockState.failCpu=true; }
            port_perf_frame_begin();
            clockState.wall=1000000000; clockState.cpu=1000000; clockState.failCpu=false;
            if (i%4==1) { clockState.wall=0; clockState.failCpu=true; }
            if (i%4==2) { --clockState.wall; --clockState.cpu; }
            port_perf_frame_work_done();
        }
        checkRow(row("whole"),256,64,0,64,0,0,192,192);
        checkRow(row("unclassified"),256,64,0,64,0,0,192,192);
        const auto h=header();
        require(number(h,"wall-faults")==128 && number(h,"cpu-faults")==128 &&
                number(h,"wall-backwards")==64 && number(h,"cpu-backwards")==64,"bad first/end clocks and valid zero-length intervals");
    } else if (scenario == "boundaries") {
        for (unsigned i=0;i<256;++i) {
            port_perf_frame_begin(); advance(900000,90); checkpoint(PORT_GAME_PHASE_DRAW);
            port_perf_frame_begin(); // Abandon the first partial turn, never publish its intervals.
            advance(10000,2); checkpoint(0);
            advance(20000,3); checkpoint(UINT32_MAX);
            advance(30000,5); checkpoint(PORT_GAME_PHASE_COUNT);
            clockState.changeErrno=true; errno=E2BIG;
            const int rounding=fegetround();
            fesetround(FE_DOWNWARD); feclearexcept(FE_ALL_EXCEPT); feraiseexcept(FE_DIVBYZERO);
            advance(40000,7); checkpoint(PORT_GAME_PHASE_DRAW);
            require(errno==E2BIG,"both checkpoint clocks preserve errno");
            require(fegetround()==FE_DOWNWARD && fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,
                    "checkpoint preserves floating-point rounding and exception state");
            feclearexcept(FE_ALL_EXCEPT); fesetround(rounding);
            clockState.changeErrno=false;
            advance(50000,11); port_perf_frame_work_done();
            const unsigned w=clockState.wallReads,c=clockState.cpuReads;
            checkpoint(PORT_GAME_PHASE_WAIT_RETRACE);
            require(clockState.wallReads==w && clockState.cpuReads==c,"inactive checkpoint has no clock reads");
            port_perf_frame_work_done();
        }
        checkRow(row("whole"),256,256,38400000,256,38400000,7168);
        checkRow(row("unclassified"),1024,1024,25600000,1024,25600000,4352);
        checkRow(row("draw"),256,256,12800000,256,12800000,2816);
        require(number(header(),"abandoned")==256 && number(header(),"unknown")==768 && number(header(),"orphan-ends")==255,
                "nested begin, unknown labels and prior orphan ends recorded");
    } else if (scenario == "threads") {
        port_perf_frame_begin();
        std::thread foreign([] {
            const unsigned w=clockState.wallReads,c=clockState.cpuReads;
            for(unsigned i=0;i<1000;++i) PORT_GAME_PROGRESS(PORT_GAME_PHASE_DRAW);
            require(clockState.wallReads==w && clockState.cpuReads==c,"foreign thread without own begin reads no clocks");
        }); foreign.join();
        advance(60000,10); port_perf_frame_work_done();
        for (unsigned i=1;i<256;++i) turn();
        std::thread owner([] { for(unsigned i=0;i<256;++i) turn(); }); owner.join();
        checkRow(row("whole",1),256,256,15360000,256,15360000,2560);
        checkRow(row("whole",2),256,256,15360000,256,15360000,2560);
        require(number(header(1),"turns")==256 && number(header(2),"turns")==256 && parses.load()==1,
                "distinct per-thread denominators share only cached option");
    } else if (scenario == "all-phases") {
        for(unsigned i=0;i<256;++i) {
            port_perf_frame_begin();
            for(uint32_t phase=1;phase<PORT_GAME_PHASE_COUNT;++phase) { advance(10000,1); checkpoint(phase); }
            advance(10000,1); port_perf_frame_work_done();
        }
        require(rows().size()==66,"maximum report is one header, whole row and 64 phase rows");
        checkRow(row("whole"),256,256,163840000,256,163840000,16384);
    } else require(false,"known scenario");
    printf("PASS phase-cpu setting=%s on=%d scenario=%s\n",argv[1],on,argv[3]);
}
