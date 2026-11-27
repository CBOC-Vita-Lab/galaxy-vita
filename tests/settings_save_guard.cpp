// Actual boundary/lease, reset-readiness query and NAND write/close sequence.
// Device calls and the guest scheduler are fixtures; save bytes stay in memory.
#include "port/settings_lifecycle.h"
#include "port/settings_ui.h"
#include "vgx_common.h"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

using namespace port::settings;
enum SaveState {
    SaveDataHandleSequenceNoOperation, SaveDataHandleSequenceSaveConfirm,
    SaveDataHandleSequenceSaveDoneKeyWait, SaveDataHandleSequenceNoSaveConfirmRemind,
    SaveDataHandleSequencePreLoad, SaveDataHandleSequencePreLoadDone,
    SaveDataHandleSequenceSave, SaveDataHandleSequenceSaveWindowDisappear,
    SaveDataHandleSequenceSaveAllWithoutKeyWait, SaveDataHandleSequenceSaveAllWithoutKeyWaitDisappear,
    SaveDataHandleSequenceSaveAllWithoutWindow, SaveDataHandleSequenceCheckEnableToCreate,
    SaveDataHandleSequenceErrorHandling, UnknownState
};
struct ErrorSequence { bool ready = true; bool isPreparedReset() const { return ready; } };
struct SaveDataHandleSequence {
    SaveState state = SaveDataHandleSequenceNoOperation;
    ErrorSequence* mNANDErrorSequence = nullptr;
    bool isNerve(SaveState value) const { return state == value; }
    bool isPreparedReset() const;
};
static SaveDataHandleSequence sequence;
static SaveDataHandleSequence* currentSequence = &sequence;
static unsigned readinessQueries = 0;
static SaveDataHandleSequence* getSaveDataHandleSequence() { ++readinessQueries; return currentSequence; }
#define GET_NERVE_ANON(name) name
#include "settings_save_readiness_actual.inc"
#undef GET_NERVE_ANON

static std::atomic<bool> sRequested{false}, sActive{false};
static std::atomic<unsigned> sRequestSeq{0};  // (polish23: a new request starts a new series of attempts)
extern "C" void port_settings_request(void) { if (!sRequested.exchange(true)) sRequestSeq.fetch_add(1); }
static bool MpRefuseSettings() { return false; }  // (MP3: the multiplayer refusal in settings_runtime.cpp: no session on the host)
static bool (*port_settings_native_request)(int) = nullptr;  // (settingsnative: no 3.0 menu on the host: the old behaviour)
static void (*port_par_plant_close_early)(void) = nullptr;  // (GXV_PAR_PLANT_EARLY: weak in the game, absent here)
static HoldGate sInputHold;
static uint64_t sRetryAfter = 0;
static std::string sStatus;
static int paused = 0, frozen = 0, interrupts = 1, audioHeld = 0;
static unsigned acquired = 0, menus = 0, restarts = 0;
static bool rendererHeld = false, failRenderer = false, failAudio = false, audioDead = false, failMemory = false;
static unsigned reserves = 0, reservedNow = 0;
static ExitIntent intent = ExitIntent::Return;
static uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static int sceKernelDelayThread(unsigned us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); return 0; }
static void port_log_summary(const char*, ...) {}
static int OSDisableInterrupts() { ++acquired; const auto old = interrupts; interrupts = 0; return old; }
static int OSRestoreInterrupts(int value) { const auto old = interrupts; interrupts = value; return old; }
extern "C" int port_settings_time_pause(int value) { const auto old = frozen; frozen = value; return old; }
static int port_is_paused() { return paused; }
static void port_set_paused(int value) { paused = value; }
static int port_vita_audio_is_held() { return audioHeld; }
static int port_vita_audio_hold(int value, unsigned) { if (value && failAudio) return 0; audioHeld = value; return 1; }
static int port_vita_audio_failed() { return audioDead; }
static int port_vgx_settings_hold(unsigned, unsigned) { rendererHeld = !failRenderer; return rendererHeld; }
static void port_vgx_settings_release(unsigned) { rendererHeld = false; }
static void port_input_settings_rearm() {}
static void port_vita_input_settings_rearm() {}
static int port_boot_title_is_visible() { return 1; }
static int port_vgx_vita_splash_active() { return 0; }
struct PortHostAllocScope { PortHostAllocScope() {} ~PortHostAllocScope() {} };
// (polish23: the menu's memory is reserved before the lease; Release gives it back whether or not the menu ran)
struct Screen {
    const char* why = "";
    bool reserved = false;
    bool Reserve() {
        if (reserved) return true;
        ++reserves;
        if (failMemory) { why = "no memory (test)"; return false; }
        reserved = true; ++reservedNow; return true;
    }
    bool Acquire(bool) { return Reserve(); }
    bool Release() { if (reserved) { reserved = false; --reservedNow; } return true; }
};
static RunResult MenuAndRetire(Screen&, OpenMode) {
    assert(rendererHeld && paused && frozen && (audioHeld || audioDead) && interrupts == 0);
    ++menus;
    return {intent, true};
}
static int sceAppMgrLoadExec(const char*, char* const*, void*) { ++restarts; return -1; }
#include "settings_save_runtime_actual.inc"

using s32 = int32_t;
using u32 = uint32_t;
constexpr int NAND_RESULT_OK = 0, NAND_RESULT_EXISTS = -6, NAND_ACCESS_READ = 1, NAND_ACCESS_WRITE = 2, NAND_RESULT_UNKNOWN = -64;
struct NANDFileInfo { int fileDescriptor = 1, accType = NAND_ACCESS_WRITE; unsigned char mark = 0; char origPath[64] = {}; };
// (the 3.0 NAND write has two writers, the in-place one and the atomic one: this test is about the settings gate around the in-place one, so the atomic
// writer's own helpers answer "not in use")
static const unsigned char kAtomicMark = 0xA7;
static bool takeAtomicOpen(int, std::string*) { return false; }
static bool nandSafeOn() { return false; }
static std::mutex sSafeMutex;
static std::map<int, std::string> sSafeOpenHp;
static void safeFinishWrite(const std::string&, bool) {}
static std::string hostPath(const char* p) { return p ? p : ""; }
static bool mp_save_is_gamedata(const std::string&) { return false; }
static bool mp_save_commit_tmp(const char*, bool (*)(const std::string&)) { return true; }
struct NANDRequestInfo {
    std::atomic<int> _40{1};
    int mType = 2, mResult = 0, mFsBlock = 16, mPermission = 0, mAttribute = 0, mInode = 0;
    const char* mPath = "synthetic", *mMoveDestDir = nullptr;
    const void* mWriteBuf = nullptr;
    u32* mCheckAnswer = nullptr;
    void (*_54)(NANDRequestInfo*) = nullptr;
};
struct NANDManagerThread {
    int mQueue = 0;
    void* run();
    void executeWriteSequence(NANDRequestInfo*);
    void executeReadSequence(NANDRequestInfo*) { assert(false); }
};
static NANDRequestInfo request;
static std::mutex ioMutex;
static std::condition_variable ioCv;
static unsigned ioPhase = 0, allowedPhase = 0;
static bool failWrite = false;
static unsigned syncs = 0, closes = 0;
static void IoPause(unsigned phase) {
    std::unique_lock<std::mutex> lock(ioMutex);
    ioPhase = phase; ioCv.notify_all();
    ioCv.wait(lock, [phase] { return allowedPhase >= phase; });
}
static s32 MemoryWrite(int, const void*, u32 n) { IoPause(1); if (failWrite) { errno = EIO; return -1; } return n; }
static int MemorySync(int) { ++syncs; IoPause(2); return 0; }
static int MemoryClose(int) { ++closes; IoPause(3); return 0; }
template<class F> static void hostJob(const char*, F& f) { f(); }
static int errnoToNand(int e) { return -e; }
static int NANDCreate(const char*, int, int) { return NAND_RESULT_EXISTS; }
static int NANDOpen(const char*, NANDFileInfo*, int) { return NAND_RESULT_OK; }
static int NANDDelete(const char*) { assert(false); return -1; }
static int NANDMove(const char*, const char*) { assert(false); return -1; }
static int NANDCheck(int, int, u32*) { assert(false); return -1; }
using OSMessage = void*;
constexpr int OS_MESSAGE_BLOCK = 1;
struct WorkerFinished {};
static void OSInitFastCast() {}
static void OSReceiveMessage(int*, OSMessage* msg, int) {
    static bool delivered = false;
    if (delivered) throw WorkerFinished{};
    delivered = true; *msg = &request;
}
#define write MemoryWrite
#define fsync MemorySync
#define close MemoryClose
void mp_save_note_write(int) {}   // (platform/src/mp/mp_savefile.cpp: the count of writes in flight; the extracted NAND hook calls it)
#include "settings_save_nand_actual.inc"
#undef write
#undef fsync
#undef close

static void AssertUnheld() {
    assert(!sActive && !sInputHold.Requested() && !rendererHeld && !paused && !frozen && !audioHeld && interrupts == 1 && reservedNow == 0);
}
static void Deferred() {
    const unsigned before = menus, holds = acquired;
    port_settings_game_boundary(1);
    assert(menus == before && acquired == holds && sRequested);
    AssertUnheld();
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    unsetenv("GXV_SETTINGS_SAVE_GUARD");
    setenv("GXV_SETTINGS", mode == "disabled" ? "0" : "1", 1);
    const bool guarded = mode == "on" || mode == "read-once-on" || mode == "failed-save";
    if (guarded) setenv("GXV_SETTINGS_SAVE_GUARD", "1", 1);
    if (mode == "zero" || mode == "read-once-off") setenv("GXV_SETTINGS_SAVE_GUARD", "0", 1);
    if (mode == "invalid") setenv("GXV_SETTINGS_SAVE_GUARD", "invalid", 1);
    std::atomic<bool> stopInput{false};
    std::thread input([&] {
        while (!stopInput) {
            const auto generation = sInputHold.Requested();
            if (generation) sInputHold.Acknowledge(generation);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    sequence.state = SaveDataHandleSequenceSave;
    sRequested = true;
    if (mode == "retry" || mode == "memory" || mode == "audio-failed") {
        // (polish23) A request is never dropped after three failures: it is tried again (backoff), holds nothing while the memory is
        // missing, and opens once what was missing is there. An output that failed for good does not keep the menu shut.
        sequence.state = SaveDataHandleSequenceNoOperation;
        port_settings_request();  // (a new request: its series of attempts)
        sRequested = true;
        failRenderer = mode == "retry";
        failMemory = mode == "memory";
        failAudio = mode == "audio-failed";
        for (unsigned attempt = 0; attempt < 6; ++attempt) {
            const unsigned holds = acquired;
            sRetryAfter = 0;
            port_settings_game_boundary(1);
            assert(menus == 0 && sRequested);
            assert(sRetryAfter > Now());  // (the next try is scheduled)
            if (mode == "memory") assert(acquired == holds);  // (nothing of the game was held for a missing buffer)
            AssertUnheld();
        }
        failRenderer = failMemory = false;
        if (mode == "audio-failed") audioDead = true;  // (still no hold from the device; it has failed: nothing to silence)
        sRetryAfter = 0;
        port_settings_game_boundary(1);
        assert(menus == 1 && !sRequested); AssertUnheld();
        std::printf("PASS: actual settings save guard %s (reserves %u)\n", mode.c_str(), reserves);
        stopInput = true; input.join();
        return 0;
    }
    if (mode == "disabled") {
        Deferred(); assert(!readinessQueries);
    } else if (!guarded) {
        port_settings_game_boundary(1);
        assert(menus == 1 && !sRequested && !readinessQueries); AssertUnheld();
        if (mode == "read-once-off") {
            setenv("GXV_SETTINGS_SAVE_GUARD", "1", 1); sRequested = true;
            port_settings_game_boundary(1);
            assert(menus == 2 && !sRequested && !readinessQueries); AssertUnheld();
        }
    } else {
        Deferred(); assert(readinessQueries);
        if (mode == "read-once-on") setenv("GXV_SETTINGS_SAVE_GUARD", "0", 1);
        const SaveState busy[] = {SaveDataHandleSequencePreLoad, SaveDataHandleSequencePreLoadDone,
            SaveDataHandleSequenceSave, SaveDataHandleSequenceSaveWindowDisappear,
            SaveDataHandleSequenceSaveAllWithoutKeyWait, SaveDataHandleSequenceSaveAllWithoutKeyWaitDisappear,
            SaveDataHandleSequenceSaveAllWithoutWindow, UnknownState};
        for (const auto state : busy) { sequence.state = state; Deferred(); }
        ErrorSequence error{false}; sequence.mNANDErrorSequence = &error;
        sequence.state = SaveDataHandleSequenceErrorHandling; Deferred();
        sequence.state = SaveDataHandleSequenceNoOperation; Deferred();
        sequence.mNANDErrorSequence = nullptr;
        // Idle -> pre-save is safe only before the game queues a transaction.
        sequence.state = SaveDataHandleSequenceSaveConfirm;
        port_settings_game_boundary(1); assert(menus == 1 && !sRequested); AssertUnheld();
        sRequested = true; sequence.state = SaveDataHandleSequenceSave;
        failWrite = mode == "failed-save";
        std::thread worker([] { NANDManagerThread manager; try { manager.run(); } catch (WorkerFinished&) {} });
        for (unsigned phase = 1; phase <= 3; ++phase) {
            { std::unique_lock<std::mutex> lock(ioMutex); ioCv.wait(lock, [phase] { return ioPhase == phase; }); }
            assert(request._40 == 2); Deferred();
            { std::lock_guard<std::mutex> lock(ioMutex); allowedPhase = phase; ioCv.notify_all(); }
        }
        worker.join(); assert(request._40 == 0 && syncs == 1 && closes == 1);
        assert(failWrite ? request.mResult < 0 : request.mResult == 0);
        Deferred(); // Completion still awaits the guest save-state update.
        sequence.state = failWrite ? SaveDataHandleSequenceErrorHandling : SaveDataHandleSequenceSaveDoneKeyWait;
        sequence.mNANDErrorSequence = failWrite ? &error : nullptr;
        if (failWrite) Deferred();
        error.ready = true;
        intent = ExitIntent::Restart;
        port_settings_game_boundary(1);
        assert(menus == 2 && restarts == 1 && sRequested); AssertUnheld(); // Failed restart requests another menu.
        sequence.state = SaveDataHandleSequenceNoOperation; sequence.mNANDErrorSequence = nullptr;
        intent = ExitIntent::Return; failRenderer = true;
        port_settings_game_boundary(1); assert(menus == 2 && sRequested); AssertUnheld();
        failRenderer = false; sRetryAfter = 0;
        port_settings_game_boundary(1); assert(menus == 3 && !sRequested); AssertUnheld();
        currentSequence = nullptr; sRequested = true;
        port_settings_game_boundary(1); assert(menus == 4 && !sRequested); AssertUnheld();
    }
    stopInput = true; input.join();
    std::printf("PASS: actual settings save guard %s\n", mode.c_str());
}
