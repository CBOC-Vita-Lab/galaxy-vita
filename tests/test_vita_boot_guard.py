"""Host regressions for the real loading renderer and host warm-up option block."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
BASE = "392ee7009fe1152b5bd397e6398f00404303a264"
HOST = "platform/vgx/vita/vgx_host_galaxy_vita.cpp"
LOADING = "platform/vgx/vita/loading_screen.h"


def warm_adapter(source):
    """Compile production statements, substituting only their external boundaries."""
    begin = source.index("void EnvLong(")
    end = source.index("\n}", begin) + 2
    env_long = source[begin:end]
    begin = source.index("        long defer = 1, budgetMs = 25, afterTitle =")
    end = source.index('\n    }\n    port_log("[vgx-vita] shader warm-up list:', begin)
    options = "\nvoid PrepareWarmOptions() {\n" + source[begin:end] + "\n}\n"
    begin = source.index("bool g_heldForShaders = false;")
    end = source.index("\nvoid Fail(", begin)
    title = "\nnamespace splash {\n" + source[begin:end] + "\n}\n"
    return env_long + title + options


HARNESS = r'''
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>
#include "loading_under_test.h"
#if __has_include("vgx_boot_guard.h")
#include "vgx_boot_guard.h"
#endif

namespace splash {
enum { kOff, kGl };
std::atomic<int> g_phase{kGl};
std::atomic<bool> g_titleSeen{false};
std::atomic<bool> g_failed{false};
std::atomic<unsigned long long> g_titleSeenUs{0};
int taskTotal = 0;
void TaskProgress(const char*, int, int total, bool = true) { taskTotal = total; }
}
namespace vgx {
bool deferred = false;
void SetShaderWarmupDeferred(bool value) { deferred = value; }
namespace backend {
double budget = -1;
unsigned pending = 2, total = 3;
void SetWarmBudget(double value) { budget = value; }
unsigned WarmPending() { return pending; }
unsigned WarmTotal() { return total; }
}
}
void port_log(const char*, ...) {}
unsigned NowMs() { return 100; }
unsigned long long NowUs() { return 100000; }
@WARM@

template<class T, class = void> struct HasReady : std::false_type {};
template<class T> struct HasReady<T, std::void_t<decltype(std::declval<T>().titleReady)>> : std::true_type {};
template<class T> void SetReady(T& state, bool value) {
    if constexpr (HasReady<T>::value) state.titleReady = value;
}
unsigned long long Hash(const std::vector<uint32_t>& pixels) {
    unsigned long long result = 1469598103934665603ull;
    for (uint32_t pixel : pixels) {
        for (unsigned shift = 0; shift != 32; shift += 8) {
            result ^= (pixel >> shift) & 255;
            result *= 1099511628211ull;
        }
    }
    return result;
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    if (std::strcmp(argv[1], "hold-cycle") == 0) {
        PrepareWarmOptions();
        splash::NoteTitleVisible("test title");
        std::printf("%.0f %d %d ", vgx::backend::budget, int(splash::g_titleSeen.load()), splash::taskTotal);
        vgx::backend::pending = 0;
        splash::NoteTitleVisible("test title");
        std::printf("%d %d\n", int(splash::g_titleSeen.load()), splash::taskTotal);
        return 0;
    }
    if (std::strcmp(argv[1], "warm") == 0) {
        if (argc > 2) splash::g_phase.store(std::atoi(argv[2]));
        PrepareWarmOptions();
        std::printf("%d %d %.0f\n", vgx::deferred, splash::g_warmAfterTitle, vgx::backend::budget);
        return 0;
    }
    const bool latch = std::strcmp(argv[1], "render-latch") == 0;
    if ((std::strcmp(argv[1], "render") == 0 || latch) && argc == 6) {
        // An explicit local backdrop avoids filesystem dependencies in the pixel oracle.
        loading::detail::g_bg = static_cast<uint32_t*>(std::malloc(loading::detail::kBackgroundBytes));
        if (!loading::detail::g_bg) return 3;
        for (int i = 0; i < loading::W * loading::H; ++i)
            loading::detail::g_bg[i] = loading::detail::Pack(i % 255, (i / 7) % 255, (i / 19) % 255);
        loading::State state;
        state.progress = std::strtof(argv[2], nullptr);
        SetReady(state, std::atoi(argv[3]) != 0);
        state.caption = std::atoi(argv[3]) ? "Ready" : "Almost there";
        state.fade = std::strtof(argv[4], nullptr);
        state.alert = std::atoi(argv[5]) != 0;
        state.footer = "A loading status";
        std::vector<uint32_t> pixels(loading::W * loading::H);
        loading::Render(pixels.data(), 123.0, state);
        std::printf("%llu", Hash(pixels));
        if (latch) {
            const char* current = std::getenv("GXV_BOOTGUARD");
            setenv("GXV_BOOTGUARD", current && std::strcmp(current, "1") == 0 ? "0" : "1", 1);
            loading::Render(pixels.data(), 124.0, state);
            std::printf(" %llu", Hash(pixels));
        }
        std::puts("");
        loading::Release();
        return 0;
    }
    return 2;
}
'''


class BootGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get("CXX", "clang++")
        if not shutil.which(compiler):
            raise unittest.SkipTest("a host C++17 compiler is required")
        cls.temp = tempfile.TemporaryDirectory(prefix="boot-guard-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binaries = {}
        for revision in ("production", "legacy"):
            directory = Path(cls.temp.name) / revision
            directory.mkdir()
            if revision == "legacy":
                host = subprocess.check_output(["git", "show", f"{BASE}:{HOST}"], cwd=ROOT, text=True)
                loading = subprocess.check_output(["git", "show", f"{BASE}:{LOADING}"], cwd=ROOT, text=True)
            else:
                host = (ROOT / HOST).read_text()
                loading = (ROOT / LOADING).read_text()
            (directory / "loading_under_test.h").write_text(loading)
            (directory / "adapter.cpp").write_text(HARNESS.replace("@WARM@", warm_adapter(host)))
            binary = directory / "boot-guard"
            command = [compiler, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                       "-I" + str(ROOT / "platform/vgx/vita"), "-I" + str(ROOT / "platform/vgx/src"),
                       str(directory / "adapter.cpp"), "-o", str(binary)]
            subprocess.run(command, check=True, cwd=ROOT, capture_output=True, text=True)
            cls.binaries[revision] = binary

    def run_adapter(self, arguments, env=None, revision="production"):
        environment = {key: value for key, value in os.environ.items() if not key.startswith("GXV_")}
        environment.update({"ASAN_OPTIONS": "detect_leaks=" + ("0" if sys.platform == "darwin" else "1"),
                            "UBSAN_OPTIONS": "halt_on_error=1"})
        environment.update(env or {})
        result = subprocess.run([str(self.binaries[revision]), *map(str, arguments)],
                                env=environment, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        return result.stdout.strip()

    def render(self, progress, ready=0, fade=0, alert=0, env=None, revision="production"):
        return self.run_adapter(["render", progress, ready, fade, alert], env, revision)

    def test_incomplete_progress_does_not_round_to_100(self):
        for progress in (0.995, 0.999, 1, 2, "inf"):
            with self.subTest(progress=progress):
                self.assertEqual(self.render(progress, env={"GXV_BOOTGUARD": "1"}),
                                 self.render(0.99, revision="legacy"))

    def test_guard_off_pixels_match_public_base(self):
        for flag in (None, "0"):
            env = {} if flag is None else {"GXV_BOOTGUARD": flag}
            for progress in ("nan", "-inf", -1, 0, 0.5, 0.99, 0.995, 1, 2, "inf"):
                for ready, fade, alert in ((0, 0, 0), (0, 0, 1), (1, 0, 0), (1, 0.5, 0), (1, 1, 0)):
                    with self.subTest(flag=flag, progress=progress, ready=ready, fade=fade, alert=alert):
                        self.assertEqual(self.render(progress, ready, fade, alert, env),
                                         self.render(progress, ready, fade, alert, revision="legacy"))

    def test_ready_and_fade_retain_real_completion(self):
        for progress in (0.995, 1, "inf"):
            for fade in (0, 0.25, 1):
                with self.subTest(progress=progress, fade=fade):
                    self.assertEqual(self.render(progress, 1, fade, env={"GXV_BOOTGUARD": "1"}),
                                     self.render(progress, 1, fade, revision="legacy"))

    def test_guarded_malformed_progress_stays_defined(self):
        for progress in ("nan", "-nan", "-inf", -1):
            with self.subTest(progress=progress):
                self.assertEqual(self.render(progress, env={"GXV_BOOTGUARD": "1"}),
                                 self.render(0, revision="legacy"))

    def test_guarded_ordinary_progress_is_unchanged(self):
        for progress in (0, 0.25, 0.5, 0.98, 0.99):
            with self.subTest(progress=progress):
                self.assertEqual(self.render(progress, env={"GXV_BOOTGUARD": "1"}),
                                 self.render(progress, revision="legacy"))

    def test_guard_flag_is_read_once(self):
        for flag, progress in (("0", 0.995), ("1", 0.99)):
            with self.subTest(flag=flag):
                expected = self.render(progress, revision="legacy")
                self.assertEqual(self.run_adapter(["render-latch", 0.995, 0, 0, 0], {"GXV_BOOTGUARD": flag}),
                                 expected + " " + expected)

    def test_guard_flag_uses_vgx_integer_convention(self):
        for flag, progress in (("bad", 0.995), ("", 0.995), ("0x1", 0.99), ("-1", 0.99)):
            with self.subTest(flag=flag):
                self.assertEqual(self.render(0.995, env={"GXV_BOOTGUARD": flag}),
                                 self.render(progress, revision="legacy"))

    def test_hold_overrides_after_title_when_guarded(self):
        env = {"GXV_BOOTGUARD": "1", "GXV_WARM_HOLD": "1", "GXV_WARM_AFTER_TITLE": "1"}
        self.assertEqual(self.run_adapter(["warm"], env), "1 0 25")
        for budget in (1, 25, 1000):
            with self.subTest(budget=budget):
                self.assertEqual(self.run_adapter(["warm"], dict(env, GXV_WARM_MS=str(budget))),
                                 f"1 0 {budget}")
        self.assertEqual(self.run_adapter(["warm"], dict(env, GXV_WARM_AFTER_TITLE="2")), "1 0 25")

    def test_other_warm_options_match_public_base(self):
        for flag in (None, "0", "1"):
            for hold in (None, "0", "1", "10", " 1"):
                for after in (None, "0", "1", "bad", "-1", "2"):
                    env = {key: value for key, value in (("GXV_BOOTGUARD", flag), ("GXV_WARM_HOLD", hold),
                                                         ("GXV_WARM_AFTER_TITLE", after)) if value is not None}
                    # EnvLong clamps AFTER_TITLE=2 to 1 rather than rejecting it.
                    conflict = flag == "1" and hold in ("1", "10") and after in ("1", "2")
                    if conflict:
                        continue
                    with self.subTest(env=env):
                        self.assertEqual(self.run_adapter(["warm"], env), self.run_adapter(["warm"], env, "legacy"))

    def test_disabled_warm_paths_are_not_enabled_by_guard(self):
        for overrides, phase in (({"GXV_WARM_DEFER": "0"}, 1), ({"GXV_NO_SHADERLIST": "1"}, 1), ({}, 0)):
            env = dict(overrides, GXV_BOOTGUARD="1", GXV_WARM_HOLD="1", GXV_WARM_AFTER_TITLE="1")
            with self.subTest(env=env, phase=phase):
                self.assertEqual(self.run_adapter(["warm", phase], env), "0 0 -1")

    def test_real_title_hold_still_waits_for_pending_shaders(self):
        env = {"GXV_BOOTGUARD": "1", "GXV_WARM_HOLD": "1", "GXV_WARM_AFTER_TITLE": "1"}
        self.assertEqual(self.run_adapter(["hold-cycle"], env), "25 0 3 1 0")
        for flag in (None, "0"):
            legacy_env = {"GXV_WARM_HOLD": "1", "GXV_WARM_AFTER_TITLE": "1"}
            if flag is not None:
                legacy_env["GXV_BOOTGUARD"] = flag
            with self.subTest(flag=flag):
                self.assertEqual(self.run_adapter(["hold-cycle"], legacy_env),
                                 self.run_adapter(["hold-cycle"], legacy_env, "legacy"))

    def test_real_default_title_path_still_starts_background_warmup(self):
        env = {"GXV_BOOTGUARD": "1"}
        self.assertEqual(self.run_adapter(["hold-cycle"], env), "10 1 0 1 0")


if __name__ == "__main__":
    unittest.main()
