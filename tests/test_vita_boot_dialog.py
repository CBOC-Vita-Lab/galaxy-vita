"""Production draw and display adapters for the optional startup dialog handoff."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASE = "392ee7009fe1152b5bd397e6398f00404303a264"
HOST = "platform/vgx/vita/vgx_host_galaxy_vita.cpp"
DRAW = "decomp/src/Game/System/SaveDataHandleSequence.cpp"


def function(source, signature):
    start = source.index(signature + " {")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


HARNESS = r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include "vgx_common.h"
#if !defined(LEGACY) && __has_include("port/boot_dialog.h")
#include "port/boot_dialog.h"
#else
namespace port::bootdialog {
bool Enabled() { return false; }
bool Requested() { return false; }
void Enable(bool) {}
}
#endif
bool OtherEnabled();
bool OtherRequested();
void OtherDrawn();
unsigned drawCalls = 0, requestedBeforeDraw = 0;
struct SysInfoWindow {
    enum { Type_Key, Type_Blocking, Type_YesNo };
    int mType = Type_YesNo;
    bool waiting = true, dead = false, hidden = false, manager = true;
    bool isWait() const { return waiting; }
    void draw() const { ++drawCalls; requestedBeforeDraw += port::bootdialog::Requested(); }
};
namespace MR {
bool isExecuteDrawLayout(const SysInfoWindow* w) { return !w->dead && !w->hidden && w->manager; }
}
struct SaveDataHandleSequence {
    SysInfoWindow* mSysInfoWindowConfirm = nullptr;
    SysInfoWindow* mSysInfoWindowSave = nullptr;
    SysInfoWindow* mSaveIcon = nullptr;
    void draw() const;
};
@DRAW@

unsigned gamePresents = 0, splashPresents = 0, swaps = 0, finishes = 0, falseFinishes = 0;
unsigned directCloses = 0;
bool g_fixedJob = false, g_wasSplash = false;
uint64_t NowUs() { return 1000000; }
unsigned NowMs() { return 1000; }
void port_log(const char*, ...) {}
@ENV_LONG@
constexpr int GL_FALSE = 0;
void vglSwapBuffers(int) { ++swaps; }
namespace loading {
constexpr int W = 960, H = 544;
namespace direct { void Close() { ++directCloses; } }
}
namespace vgx::backend {
bool SetPicture(const uint32_t*, int, int) { return true; }
bool DrawPicture() { ++splashPresents; return true; }
unsigned pending = 0;
unsigned WarmPending() { return pending; }
unsigned WarmTotal() { return 1; }
void SetWarmBudget(double) {}
}
namespace splash {
@PHASES@
constexpr int kShaders = 4;
std::atomic<int> g_phase{kGl}, g_stagePermille{0}, g_stage{0};
std::atomic<bool> g_titleSeen{false}, g_failed{false}, g_faded{false};
std::atomic<uint64_t> g_titleSeenUs{0};
bool g_heldForShaders = false, g_warmAfterTitle = false;
std::atomic<uint32_t> g_frameId{1};
std::mutex g_picLock;
uint32_t* g_pic[2]{};
int g_front = 0, g_blackPresents = 0;
uint32_t g_uploadedId = 0;
uint64_t g_upUs = 0, g_drawUs = 0, g_swapUs = 0, g_presented = 0;
uint64_t g_lastPresentUs = 0, g_freeBlockAtUs = 0;
@ACTIVE@
void Enter(int) {}
void TaskProgress(const char*, int, int, bool = true) {}
@TITLE@
void Finish() {
    ++finishes;
    falseFinishes += !g_titleSeen.load();
    g_phase.store(kDone);
    port::bootdialog::Enable(false);
}
@CONFIGURE@
@HANDLE@
@PRESENT@
@SERVICE@
uint32_t Cadence() {
@CADENCE@
    return periodUs;
}
}
@SHOW@
@IDLE@
@IDLE_GAP@
@NEEDS_SERVICE@
void RendererIdle() {
    if (port_vgx_vita_splash_active()) port_vgx_vita_idle();
}
void PresentDisplayDecision() {
    struct { unsigned drawCallsThisFrame = 36; } stats;
@DECISION@
    ++gamePresents;
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    splash::ConfigureDialog();
    if (std::string(argv[1]) == "cross-tu") {
        std::printf("%d %d ", OtherEnabled(), OtherRequested());
        OtherDrawn();
        std::printf("%d ", port::bootdialog::Requested());
        port::bootdialog::Enable(false);
        std::printf("%d\n", OtherEnabled());
        return 0;
    }
    if (std::string(argv[1]) == "cadence" && argc == 5) {
        splash::g_phase.store(std::atoi(argv[2]));
        splash::g_stage.store(std::atoi(argv[3]));
        splash::g_titleSeen.store(std::atoi(argv[4]));
        std::printf("%u\n", splash::Cadence());
        return 0;
    }
    SysInfoWindow window;
    SaveDataHandleSequence owner;
    owner.mSysInfoWindowConfirm = &window;
    if (std::string(argv[1]) == "windows") {
        owner.mSysInfoWindowConfirm = nullptr;
        if (argc > 2) {
            owner.mSysInfoWindowSave = &window;
            owner.mSaveIcon = &window;
        }
        owner.draw();
        std::printf("%d %u\n", port::bootdialog::Requested(), drawCalls);
        return 0;
    }
    if (std::string(argv[1]) == "draw" && argc == 7) {
        window.mType = std::atoi(argv[2]);
        window.waiting = std::atoi(argv[3]);
        window.dead = std::atoi(argv[4]);
        window.hidden = std::atoi(argv[5]);
        window.manager = std::atoi(argv[6]);
        owner.draw();
        std::printf("%d %u %u\n", port::bootdialog::Requested(), drawCalls, requestedBeforeDraw);
        return 0;
    }
    if (std::string(argv[1]) == "config") {
        const bool before = port::bootdialog::Enabled();
        setenv("GXV_BOOTDIALOG", before ? "0" : "1", 1);
        splash::ConfigureDialog();
        std::printf("%d %d\n", before, port::bootdialog::Enabled());
        return 0;
    }
    if (std::string(argv[1]) == "direct") splash::g_phase.store(splash::kDirect);
    if (std::string(argv[1]) == "already-ready") splash::NoteTitleVisible("title logo");
    if (std::string(argv[1]) == "no-modal") window.mType = SysInfoWindow::Type_Blocking;
    if (std::string(argv[1]) == "failure-before") splash::g_failed.store(true);
    if (std::string(argv[1]) == "housekeeping") splash::g_freeBlockAtUs = 1;
    if (std::string(argv[1]) == "hold-cycle") {
        setenv("GXV_WARM_HOLD", "1", 1);
        vgx::backend::pending = 1;
    }
    owner.draw();
    PresentDisplayDecision();
    window.dead = true;
    owner.draw();
    port_vgx_vita_idle();
    port_vgx_vita_idle_gap(80000);
    ShowProgressBar(1, 2);
    splash::PresentFrame(true);
    PresentDisplayDecision();
    std::printf("%u %u %u %u %u %d %d", gamePresents, splashPresents, swaps, finishes, falseFinishes,
                splash::g_titleSeen.load(), splash::g_phase.load());
    if (std::string(argv[1]) == "cycle") {
        splash::NoteTitleVisible("title logo");
        RendererIdle();
    }
    if (std::string(argv[1]) == "hold-cycle") {
        splash::NoteTitleVisible("title logo");
        RendererIdle();
        std::printf(" | %d %u", splash::g_titleSeen.load(), finishes);
        vgx::backend::pending = 0;
        splash::NoteTitleVisible("title logo");
        RendererIdle();
    }
    if (std::string(argv[1]) == "failure" || std::string(argv[1]) == "failure-title") {
        splash::g_failed.store(true);
        if (std::string(argv[1]) == "failure-title") splash::NoteTitleVisible("title logo");
        ++splash::g_frameId;
        RendererIdle();
    }
    std::printf(" | %u %u %u %d\n", splashPresents, finishes, falseFinishes, splash::g_phase.load());
    if (std::string(argv[1]) == "housekeeping") std::printf("%u\n", directCloses);
    if (std::string(argv[1]) == "cycle") std::printf("%d\n", port::bootdialog::Enabled());
}
'''

OTHER_TU = r'''
#if !defined(LEGACY) && __has_include("port/boot_dialog.h")
#include "port/boot_dialog.h"
bool OtherEnabled() { return port::bootdialog::Enabled(); }
bool OtherRequested() { return port::bootdialog::Requested(); }
void OtherDrawn() { port::bootdialog::Drawn(); }
#else
bool OtherEnabled() { return false; }
bool OtherRequested() { return false; }
void OtherDrawn() {}
#endif
'''


class BootDialogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cxx = os.environ.get("CXX", "clang++")
        if not shutil.which(cxx):
            raise unittest.SkipTest("a host C++17 compiler is required")
        cls.temp = tempfile.TemporaryDirectory(prefix="boot-dialog-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binaries = {}
        for revision in ("production", "legacy"):
            def read(path):
                if revision == "legacy":
                    return subprocess.check_output(["git", "show", f"{BASE}:{path}"], cwd=ROOT, text=True)
                return (ROOT / path).read_text()
            host = read(HOST)
            start = host.index("    g_lastStats = stats;", host.index("void PresentDisplayCopy(unsigned efb, int x, int y, int w, int h) {"))
            start = host.index("\n", start) + 1
            boundary = "    if (!GamePresentAllowed(" if "bool GamePresentAllowed(" in host else "    if (efb == 0"
            end = host.index(boundary, start)
            code = HARNESS.replace("@DRAW@", function(read(DRAW), "void SaveDataHandleSequence::draw() const"))
            code = code.replace("@PHASES@", re.search(r"enum Phase \{[^}]+\};", host)[0])
            for tag, signature, fallback in (
                ("ENV_LONG", "void EnvLong(const char* name, long& value, long lo, long hi)", ""),
                ("ACTIVE", "inline bool Active()", ""),
                ("TITLE", "void NoteTitleVisible(const char* how)", ""),
                ("CONFIGURE", "void ConfigureDialog()", "void ConfigureDialog() {}"),
                ("HANDLE", "bool HandleDialog()", ""),
                ("PRESENT", "bool PresentFrame(bool force)", ""),
                ("SERVICE", "void Service(uint64_t minGapUs = 0)", ""),
                ("SHOW", "void ShowProgressBar(uint32_t done, uint32_t total)", ""),
                ("IDLE", 'extern "C" void port_vgx_vita_idle(void)', ""),
                ("IDLE_GAP", 'extern "C" void port_vgx_vita_idle_gap(unsigned gapUs)', ""),
                ("NEEDS_SERVICE", 'extern "C" int port_vgx_vita_splash_active(void)', ""),
            ):
                code = code.replace("@" + tag + "@", function(host, signature) if signature in host else fallback)
            code = code.replace("@DECISION@", host[start:end])
            cadence_start = host.index("        uint32_t periodUs = 33000u;")
            cadence_end = host.index("        const uint64_t used =", cadence_start)
            code = code.replace("@CADENCE@", host[cadence_start:cadence_end])
            source = Path(cls.temp.name) / (revision + ".cpp")
            source.write_text(code)
            other = Path(cls.temp.name) / (revision + "_other.cpp")
            other.write_text(OTHER_TU)
            binary = source.with_suffix("")
            command = [cxx, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-DTARGET_PC",
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I" + str(ROOT / "platform/include"),
                       "-I" + str(ROOT / "platform/vgx/src"), str(source), str(other), "-o", str(binary)]
            if revision == "legacy":
                command.append("-DLEGACY")
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stderr)
            cls.binaries[revision] = binary

    def run_adapter(self, arguments, flag="1", revision="production"):
        env = {key: value for key, value in os.environ.items() if not key.startswith("GXV_")}
        if flag is not None:
            env["GXV_BOOTDIALOG"] = flag
        env.update(ASAN_OPTIONS="detect_leaks=" + ("0" if sys.platform == "darwin" else "1"), UBSAN_OPTIONS="halt_on_error=1")
        result = subprocess.run([str(self.binaries[revision]), *map(str, arguments)], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        return result.stdout.strip()

    def test_only_actually_drawn_actionable_confirm_window_notifies(self):
        for kind in (0, 1, 2):
            for waiting in (0, 1):
                for dead, hidden, manager in ((0, 0, 1), (1, 0, 1), (0, 1, 1), (0, 0, 0)):
                    expected = int(kind in (0, 2) and waiting and not dead and not hidden and manager)
                    with self.subTest(kind=kind, waiting=waiting, dead=dead, hidden=hidden, manager=manager):
                        self.assertEqual(self.run_adapter(["draw", kind, waiting, dead, hidden, manager]), f"{expected} 1 0")

    def test_yield_survives_all_render_entries_without_fake_readiness(self):
        self.assertEqual(self.run_adapter(["cycle"]), "2 0 0 0 0 0 4 | 0 1 0 3\n0")

    def test_existing_title_shader_hold_still_delays_completion(self):
        self.assertEqual(self.run_adapter(["hold-cycle"]), "2 0 0 0 0 0 4 | 0 0 | 0 1 0 3")

    def test_host_failure_is_not_hidden_by_sticky_yield(self):
        for mode in ("failure", "failure-title"):
            with self.subTest(mode=mode):
                self.assertEqual(self.run_adapter([mode]), "2 0 0 0 0 0 4 | 1 0 0 2")

    def test_null_confirmation_and_mini_save_indicator_do_not_notify(self):
        self.assertEqual(self.run_adapter(["windows"]), "0 0")
        self.assertEqual(self.run_adapter(["windows", "mini"]), "0 2")

    def test_without_modal_and_with_existing_failure_preserve_splash(self):
        for mode in ("no-modal", "failure-before"):
            with self.subTest(mode=mode):
                self.assertEqual(self.run_adapter([mode]), self.run_adapter([mode], "0", "legacy"))

    def test_yield_closes_retired_direct_buffer_once(self):
        self.assertEqual(self.run_adapter(["housekeeping"]), "2 0 0 0 0 0 4 | 0 0 0 4\n1")

    def test_notification_is_shared_across_translation_units(self):
        self.assertEqual(self.run_adapter(["cross-tu"]), "1 0 1 0")
        self.assertEqual(self.run_adapter(["cross-tu"], "0"), "0 0 0 0")

    def test_yield_keeps_slow_ticker_cadence_and_legacy_phases_match(self):
        for stage, expected in ((0, 150000), (4, 250000)):
            self.assertEqual(self.run_adapter(["cadence", 4, stage, 0]), str(expected))
            for phase in range(4):
                for title in (0, 1):
                    args = ["cadence", phase, stage, title]
                    with self.subTest(phase=phase, stage=stage, title=title):
                        self.assertEqual(self.run_adapter(args), self.run_adapter(args, "0", "legacy"))

    def test_off_paths_match_public_base(self):
        for flag in (None, "0"):
            for mode in ("cycle", "failure", "direct", "already-ready"):
                with self.subTest(flag=flag, mode=mode):
                    self.assertEqual(self.run_adapter([mode], flag), self.run_adapter([mode], flag, "legacy"))

    def test_direct_phase_and_existing_title_are_not_yielded(self):
        for mode in ("direct", "already-ready"):
            with self.subTest(mode=mode):
                self.assertEqual(self.run_adapter([mode]), self.run_adapter([mode], "0", "legacy"))

    def test_flag_default_and_read_once(self):
        for flag, enabled in ((None, 0), ("0", 0), ("bad", 0), ("1", 1), ("0x1", 1)):
            with self.subTest(flag=flag):
                self.assertEqual(self.run_adapter(["config"], flag), f"{enabled} {enabled}")


if __name__ == "__main__":
    unittest.main()
