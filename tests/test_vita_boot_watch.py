"""Host tests for the start-up watchdog decisions (vgx_boot_watch.h) and for how the host and the renderer glue use them."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "platform/vgx/vita/vgx_host_galaxy_vita.cpp"
GLUE = ROOT / "platform/src/gx/vgx_galaxy.cpp"
DVD = ROOT / "platform/src/dvd/dvd.cpp"
PATCH = ROOT / "tools/vitagl/patches/0002-galaxy-shader-cache-names-stay-llx.patch"


def function_text(source, signature):
    """The text of a production function, from its signature to its closing brace."""
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "vgx_boot_watch.h"
using namespace vgx::bootwatch;

void EnvLong(const char* name, long& value, long lo, long hi) {
    if (const char* e = std::getenv(name); e && *e) value = std::atol(e);
    if (value < lo) value = lo;
    if (value > hi) value = hi;
}
@READSECONDS@

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++fails; } } while (0)

static void stall() {
    // No sign of life for the limit: stalled once; any change resets it and says "resumed" once.
    StallWatch w(60.0);
    const uint64_t a[3] = {1, 2, 3};
    auto r = w.Update(0, a, 3);
    CHECK(!r.stalled && !r.resumed);
    r = w.Update(59u * 1000000u, a, 3);
    CHECK(!r.stalled && r.quietSeconds > 58.9);
    r = w.Update(60u * 1000000u, a, 3);
    CHECK(r.stalled && r.becameStalled && !r.resumed);
    r = w.Update(61u * 1000000u, a, 3);
    CHECK(r.stalled && !r.becameStalled);
    const uint64_t b[3] = {1, 2, 4};
    r = w.Update(62u * 1000000u, b, 3);
    CHECK(!r.stalled && r.resumed);
    r = w.Update(63u * 1000000u, b, 3);
    CHECK(!r.stalled && !r.resumed);
    r = w.Update(121u * 1000000u, b, 3);
    CHECK(!r.stalled);                    // 59 s since the change at 62 s
    r = w.Update(122u * 1000000u, b, 3);
    CHECK(r.stalled && r.becameStalled);  // a second episode is reported again

    // Off: never stalls. A different number of words counts as a change.
    StallWatch off(0.0);
    off.Update(0, a, 3);
    CHECK(!off.Update(100000u * 1000000u, a, 3).stalled);
    StallWatch w2(10.0);
    w2.Update(0, a, 3);
    r = w2.Update(9u * 1000000u, a, 2);
    CHECK(!r.stalled && r.quietSeconds == 0.0);
    CHECK(w2.Update(19u * 1000000u, a, 2).stalled);
    // More than kSignatureWords words are cut, not overrun.
    uint64_t many[20] = {};
    StallWatch w3(5.0);
    w3.Update(0, many, 20);
    many[19] = 7;  // beyond the watched words
    CHECK(w3.Update(6u * 1000000u, many, 20).stalled);
}

static void small() {
    CHECK(FramesAdvancing(100, 130, 1.0, 20.0));
    CHECK(!FramesAdvancing(100, 119, 1.0, 20.0));
    CHECK(!FramesAdvancing(100, 90, 1.0, 20.0));   // a counter that went backwards
    CHECK(!FramesAdvancing(0, 1000, 0.0, 20.0));
    CHECK(!TitleFallbackDue(0, 1000, 1000, false));       // off
    CHECK(!TitleFallbackDue(30, 29.9, 20, false));
    CHECK(!TitleFallbackDue(30, 31, 4.9, false));         // not running pictures long enough
    CHECK(TitleFallbackDue(30, 31, 5.0, false));
    CHECK(!TitleFallbackDue(30, 31, 20, true));           // the title was seen
    CHECK(!HoldExpired(5, 5, 90));
    CHECK(!HoldExpired(0, 89u * 1000000u, 90));
    CHECK(HoldExpired(0, 90u * 1000000u, 90));
    CHECK(!HoldExpired(0, 1000u * 1000000u, 0));          // no limit
    CHECK(!ForceFinishDue(0, 100u * 1000000u, 5));        // the title was not seen
    CHECK(!ForceFinishDue(1000000, 5u * 1000000u, 5));
    CHECK(ForceFinishDue(1000000, 6u * 1000000u, 5));
    CHECK(!ForceFinishDue(1000000, 100u * 1000000u, 0));
    CHECK(ParkOnError(true, true));
    CHECK(!ParkOnError(false, true));
    CHECK(!ParkOnError(true, false));
}

static void texts() {
    CHECK(std::strcmp(DescribeSceError(0x80010002u), "file not found") == 0);
    CHECK(std::strcmp(DescribeSceError(0x8001000Du), "access denied") == 0);
    CHECK(std::strcmp(DescribeSceError(0x8001001Cu), "card full") == 0);
    CHECK(std::strcmp(DescribeSceError(0x8002D01Cu), "module loader error") == 0);
    CHECK(std::strcmp(DescribeSceError(0x12345678u), "") == 0);

    // A status call that fails is not proof that the file is missing.
    FileProbe gone{-0x7FFEFFFE, -0x7FFEFFFE}, statOnly{0, -1}, openOnly{-1, 5};
    CHECK(CompilerMissing(gone, gone));
    CHECK(!CompilerMissing(gone, statOnly));
    CHECK(!CompilerMissing(openOnly, gone));
    CHECK(!CompilerMissing(statOnly, openOnly));
    CHECK(Reachable(statOnly) && Reachable(openOnly) && !Reachable(gone));

    char out[400];
    FormatCompilerMissing(out, sizeof out, FileProbe{static_cast<int>(0x80010002u), static_cast<int>(0x80010002u)},
                          FileProbe{static_cast<int>(0x8001000Du), static_cast<int>(0x80010013u)});
    const std::string missing = out;
    CHECK(missing.find("0x80010002") != std::string::npos);
    CHECK(missing.find("0x8001000D") != std::string::npos);
    CHECK(missing.find('\n') != std::string::npos);
    FormatCompilerLoadFailure(out, sizeof out, static_cast<int>(0x8002D01Cu), static_cast<int>(0x80010002u));
    const std::string load = out;
    CHECK(load.find("0x8002D01C") != std::string::npos && load.find("0x80010002") != std::string::npos);
    CHECK(load.find("module loader error") != std::string::npos);
    for (const std::string& text : {missing, load}) {
        const size_t nl = text.find('\n');
        CHECK(nl != std::string::npos && nl <= 88 && text.size() - nl - 1 <= 88);
    }
    // Cut to the buffer, never past it.
    char small[16];
    FormatCompilerLoadFailure(small, sizeof small, -1, -2);
    CHECK(std::strlen(small) < sizeof small);

    char l1[100], l2[100];
    CHECK(WrapTwoLines("first line\nsecond line", l1, sizeof l1, l2, sizeof l2) == 2);
    CHECK(std::strcmp(l1, "first line") == 0 && std::strcmp(l2, "second line") == 0);
    CHECK(WrapTwoLines("short", l1, sizeof l1, l2, sizeof l2) == 1);
    CHECK(std::strcmp(l1, "short") == 0 && l2[0] == 0);
    const char* longText = "The shader compiler module did not load and the renderer cannot compile anything without it, so the game cannot start now";
    CHECK(WrapTwoLines(longText, l1, sizeof l1, l2, sizeof l2) == 2);
    CHECK(std::strlen(l1) <= 88 && std::strlen(l1) > 44 && longText[std::strlen(l1)] == ' ');
    CHECK(std::string(l1) + " " + l2 == longText);
    const std::string noSpaces(150, 'x');
    CHECK(WrapTwoLines(noSpaces.c_str(), l1, sizeof l1, l2, sizeof l2) == 2 && std::strlen(l1) == 88 && std::strlen(l2) == 62);
    CHECK(WrapTwoLines("a log line\n\nwith gaps\r\nand more", l1, sizeof l1, l2, sizeof l2) == 2 && std::strchr(l2, '\n') == nullptr && std::strchr(l2, '\r') == nullptr);
    CHECK(WrapTwoLines("", l1, sizeof l1, l2, sizeof l2) == 0 && l1[0] == 0);
    CHECK(WrapTwoLines(nullptr, l1, sizeof l1, l2, sizeof l2) == 0);
    CHECK(WrapTwoLines("one line only", l1, sizeof l1, nullptr, 0) == 1);

    char fl[64];
    FirstLine(fl, sizeof fl, "the vertex test shader did not compile:\nline two", 20);
    CHECK(std::strcmp(fl, "the vertex test sha") == 0 || std::strlen(fl) <= 20);
    FirstLine(fl, sizeof fl, "tab\there\nsecond", 60);
    CHECK(std::strcmp(fl, "tab?here") == 0);
    FirstLine(fl, sizeof fl, "", 60);
    CHECK(fl[0] == 0);

    char path[128];
    CHECK(ShaderCachePath(path, sizeof path, "ux0:data/smg/shadercache/0123abcd/v3", false, 0xAB00000000001234ull));
    CHECK(std::strcmp(path, "ux0:data/smg/shadercache/0123abcd/v3/v/AB/AB00000000001234.gxp") == 0);
    CHECK(ShaderCachePath(path, sizeof path, "root", true, 0x0500000000000007ull));
    CHECK(std::strcmp(path, "root/f/05/500000000000007.gxp") == 0);  // the hash is not padded: the leading zero of 0x05... is dropped
    CHECK(!ShaderCachePath(path, 10, "a-root-that-is-long", false, 1));
    CHECK(!ShaderCachePath(path, sizeof path, nullptr, false, 1));

    char tail[40];
    PathTail(tail, sizeof tail, "ux0:data/smg/game/files/AudioRes/Stream/SMG_title_strm.ast", 30);
    CHECK(std::strlen(tail) == 30 && std::strncmp(tail, "...", 3) == 0 && std::strstr(tail, "SMG_title_strm.ast"));
    PathTail(tail, sizeof tail, "short/path", 30);
    CHECK(std::strcmp(tail, "short/path") == 0);
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "seconds") == 0) {
        // ReadSeconds("NAME", fallback) as the host reads its limits
        std::printf("%ld\n", ReadSeconds(argv[2], std::atol(argv[3])));
        return 0;
    }
    stall();
    small();
    texts();
    if (!fails) std::puts("ok");
    return fails ? 1 : 0;
}
'''


class BootWatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get("CXX", "clang++")
        if not shutil.which(compiler):
            raise unittest.SkipTest("a host C++17 compiler is required")
        cls.temp = tempfile.TemporaryDirectory(prefix="boot-watch-")
        cls.addClassCleanup(cls.temp.cleanup)
        host = HOST.read_text()
        # The production ReadSeconds (the limits' reader) is compiled as it is.
        seconds = function_text(host, "long ReadSeconds(const char* name, long fallback)")
        source = Path(cls.temp.name) / "adapter.cpp"
        source.write_text(HARNESS.replace("@READSECONDS@", seconds))
        cls.binary = Path(cls.temp.name) / "boot-watch"
        subprocess.run([compiler, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-I" + str(ROOT / "platform/vgx/vita"), str(source), "-o", str(cls.binary)],
                       check=True, cwd=ROOT, capture_output=True, text=True)

    def run_binary(self, *arguments, env=None):
        environment = {k: v for k, v in os.environ.items() if not k.startswith("GXV_")}
        environment.update({"ASAN_OPTIONS": "detect_leaks=0", "UBSAN_OPTIONS": "halt_on_error=1"})
        environment.update(env or {})
        return subprocess.run([str(self.binary), *map(str, arguments)], env=environment, capture_output=True, text=True)

    def test_watchdog_decisions_and_texts(self):
        result = self.run_binary()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), "ok")

    def test_limits_are_read_with_defaults_zero_and_clamps(self):
        self.assertEqual(self.run_binary("seconds", "GXV_BOOT_STALL_SEC", 60).stdout.strip(), "60")
        self.assertEqual(self.run_binary("seconds", "GXV_BOOT_STALL_SEC", 60, env={"GXV_BOOT_STALL_SEC": "0"}).stdout.strip(), "0")
        self.assertEqual(self.run_binary("seconds", "GXV_BOOT_STALL_SEC", 60, env={"GXV_BOOT_STALL_SEC": "15"}).stdout.strip(), "15")
        self.assertEqual(self.run_binary("seconds", "GXV_BOOT_STALL_SEC", 60, env={"GXV_BOOT_STALL_SEC": "-5"}).stdout.strip(), "0")
        self.assertEqual(self.run_binary("seconds", "GXV_BOOT_STALL_SEC", 60, env={"GXV_BOOT_STALL_SEC": "99999999"}).stdout.strip(), "86400")
        self.assertEqual(self.run_binary("seconds", "GXV_BOOT_STALL_SEC", 60, env={"GXV_BOOT_STALL_SEC": ""}).stdout.strip(), "60")

    def test_cache_path_matches_the_vitagl_patch(self):
        patch = PATCH.read_text()
        self.assertIn('"%s/%s/%02X/%llX.gxp"', patch)
        self.assertIn('"%s/v/%02X/%llX.gxp"', patch)
        pattern = "%s/%s/%02X/%llX.gxp".replace("%llX", "%X")
        hashed = 0xAB00000000001234
        self.assertEqual(pattern % ("root", "v", hashed >> 56, hashed), "root/v/AB/AB00000000001234.gxp")

    # --- how the production sources use the decisions (a refactor must not silently drop a bound) ---
    def test_host_uses_every_bound(self):
        host = HOST.read_text()
        for needle in (
            "vgx::bootwatch::CompilerMissing(compilerA, compilerB)",       # the module is missing only when no call reaches it
            "vgx::bootwatch::FormatCompilerLoadFailure",                    # a failed load names both result codes
            "shark_init(nullptr)",
            "vgx::bootwatch::HoldExpired(",                                 # GXV_WARM_HOLD has a limit
            "vgx::bootwatch::ForceFinishDue(",                              # the screen cannot outlive the title for ever
            "vgx::bootwatch::StallWatch watch(",                            # a minute without any sign of life says so
            "vgx::bootwatch::TitleFallbackDue(",                            # the opt-in fallback
            'ReadSeconds("GXV_BOOT_STALL_SEC", 60)',
            'ReadSeconds("GXV_BOOT_FINISH_SEC", 5)',
            'ReadSeconds("GXV_WARM_HOLD_MAX_SEC", 90)',
            'ReadSeconds("GXV_BOOT_TITLE_FALLBACK", 0)',
            'vgx::EnvInt("GXV_SELFTEST_RETRY", 1)',
            "port_vgx_vita_error_shown",
            "port_vgx_vita_backend_failed",
        ):
            self.assertIn(needle, host)
        # No stat-only decision is left in front of the module load.
        self.assertNotIn('Exists("ur0:data/libshacccg.suprx")', host)

    def test_renderer_glue_parks_and_bounds_its_waits(self):
        glue = GLUE.read_text()
        for needle in (
            "vgx::bootwatch::ParkOnError(park, port_vgx_vita_error_shown() != 0)",
            'vgx::EnvInt("GXV_BOOT_PARK", 1)',
            'vgx::EnvInt("GXV_BOOT_WAIT_SEC", 120)',
            "GraphicsWaitTimedOut(\"EnsureThread\", limit)",
            "GraphicsWaitTimedOut(\"FrameSink\", limit)",
            "port_vgx_vita_backend_failed();",
        ):
            self.assertIn(needle, glue)
        # Every start-up "no graphics context" ends in the parking helper, not in a bare abort.
        self.assertNotIn('port_fatal("vgx: no graphics context', glue)
        self.assertEqual(glue.count("GraphicsFailed(kNoContext)"), 3)

    def test_disc_layer_keeps_its_counters_without_changing_reads(self):
        dvd = DVD.read_text()
        for needle in ("port_dvd_bytes_read", "port_dvd_failures", "port_dvd_first_failure", "port_boot_title_audio_problem",
                       "sDvdBytesRead.fetch_add((uint64_t)want", 'noteDvdFailure("open"', 'noteDvdFailure("read"'):
            self.assertIn(needle, dvd)
        # The diagnostics switch keeps its off default (tests/dvd_errors.cpp relies on it).
        self.assertIn('vgx::EnvInt("GXV_DVDERRORS", 0)', dvd)


if __name__ == "__main__":
    unittest.main()
