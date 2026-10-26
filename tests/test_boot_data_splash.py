"""Compile the actual Vita boot-error adapter with host-only display boundaries."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "platform/vgx/vita/vgx_host_galaxy_vita.cpp"


def function(source, signature):
    start = source.index(signature + " {")
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


HARNESS = r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
bool enabled;
unsigned finishes, pictures;
uint32_t pixel;
extern "C" int port_boot_data_enabled() { return enabled; }
uint64_t NowUs() { return 1000000; }
unsigned NowMs() { return 1000; }
void port_log(const char*, ...) {}
void EnvLong(const char*, long&, long, long) {}
namespace loading::direct { void* g_base = &pixel; void Close() {} }
namespace vgx::backend {
unsigned WarmPending() { return 0; }
unsigned WarmTotal() { return 0; }
void SetWarmBudget(double) {}
}
namespace splash {
@PHASES@
std::atomic<int> g_phase{kGl};
std::atomic<bool> g_failed{false}, g_titleSeen{false}, g_faded{false};
std::atomic<uint64_t> g_titleSeenUs{0};
std::mutex g_failLock;
char g_failText[200] = "";
int g_thread = 1, g_blackPresents = 0;
uint32_t* g_pic[2] = {&pixel, &pixel};
bool g_heldForShaders = false, g_warmAfterTitle = false;
uint64_t g_lastPresentUs = 0, g_freeBlockAtUs = 0;
void TaskProgress(const char*, int, int, bool = true) {}
@FAIL@
@TITLE@
bool Active() { return g_phase.load() == kGl; }
bool PresentFrame(bool) { ++pictures; return true; }
void Finish() { ++finishes; g_phase.store(kDone); }
@SERVICE@
}
@BRIDGE@
int main(int argc, char** argv) {
    if (argc != 6) return 2;
    splash::g_phase.store(std::atoi(argv[1]));
    loading::direct::g_base = std::atoi(argv[2]) ? &pixel : nullptr;
    enabled = std::atoi(argv[3]);
    splash::g_thread = std::atoi(argv[4]);
    if (!std::atoi(argv[5])) splash::g_pic[1] = nullptr;
    int accepted = port_boot_data_error("Restore converted title audio and relaunch; saves unchanged.");
    if (accepted) {
        splash::Fail("must not replace the first error");
        splash::NoteTitleVisible("late title signal");
        splash::g_faded.store(true);  // even an earlier fade must not hide a published data failure
        splash::Service(); splash::Service(); splash::Service();
    }
    std::printf("%d %d %d %u %d %s\n", accepted, splash::g_failed.load(), splash::g_titleSeen.load(),
                finishes, splash::g_phase.load(), splash::g_failText);
}
'''


class BootDataSplashTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cxx = os.environ.get("CXX", "clang++")
        if not shutil.which(cxx):
            raise unittest.SkipTest("host C++ compiler required")
        cls.temp = tempfile.TemporaryDirectory(prefix="boot-data-splash-")
        cls.addClassCleanup(cls.temp.cleanup)
        host = HOST.read_text()
        code = HARNESS.replace("@PHASES@", re.search(r"enum Phase \{[^}]+\};", host)[0])
        for tag, signature in (
            ("FAIL", "void Fail(const char* what)"),
            ("TITLE", "void NoteTitleVisible(const char* how)"),
            ("SERVICE", "void Service(uint64_t minGapUs = 0)"),
            ("BRIDGE", 'extern "C" int port_boot_data_error(const char* message)'),
        ):
            code = code.replace("@" + tag + "@", function(host, signature))
        # WP-S's renderer-owned yield handoff is a separate production adapter test.
        if "HandleDialog()" in code:
            code = code.replace("bool Active()", "bool HandleDialog() { return false; }\nbool Active()")
        source = Path(cls.temp.name) / "adapter.cpp"
        source.write_text(code)
        cls.binary = source.with_suffix("")
        result = subprocess.run([cxx, "-std=c++17", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                 str(source), "-o", str(cls.binary)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)

    def adapter(self, phase, direct=1, enabled=1, thread=1, pictures=1):
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=" + ("0" if sys.platform == "darwin" else "1"), UBSAN_OPTIONS="halt_on_error=1")
        result = subprocess.run([str(self.binary), *map(str, (phase, direct, enabled, thread, pictures))],
                                capture_output=True, text=True, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        return result.stdout.strip()

    def test_direct_without_scanout_is_not_visible(self):
        self.assertEqual(self.adapter(1, direct=0), "0 0 0 0 1")

    def test_disabled_unavailable_or_finished_never_accepts(self):
        for phase, enabled, thread, pictures in ((0, 1, 1, 1), (3, 1, 1, 1), (2, 0, 1, 1), (2, 1, -1, 1), (2, 1, 1, 0)):
            with self.subTest(phase=phase, enabled=enabled, thread=thread, pictures=pictures):
                self.assertEqual(self.adapter(phase, enabled=enabled, thread=thread, pictures=pictures), f"0 0 0 0 {phase}")

    def test_published_error_survives_title_and_preexisting_fade(self):
        for phase in (1, 2, 4):  # WP-S appends yielded=4; off/done values stay unchanged.
            with self.subTest(phase=phase):
                self.assertEqual(self.adapter(phase), f"1 1 0 0 {phase} Restore converted title audio and relaunch; saves unchanged.")

    def test_ticker_ready_text_is_gated_by_failed_state(self):
        self.assertIn("if (g_titleSeen.load() && !g_failed.load()) {", HOST.read_text())


if __name__ == "__main__":
    unittest.main()
