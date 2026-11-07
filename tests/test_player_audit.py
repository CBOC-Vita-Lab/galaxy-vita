"""Source-level guards for the player-build audit: no timer reads per draw, the pthread link guard, the frame-clock switch."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
BACKEND = ROOT / "platform/vgx/src/vgx_backend_gl.cpp"
VGX_SRC = ROOT / "platform/vgx/src"
CLOCK_CALL = re.compile(r"\b(NowMs|DNow|Now|NowUs|NowNs|sceKernelGetProcessTime\w*|sceKernelGetSystemTime\w*|sceKernelGetThreadInfo|"
                        r"port_host_time_ns|port_thread_cpu_us|clock_gettime|gettimeofday)\s*\(|steady_clock")


def function_body(text, signature):
    start = text.index(signature)
    open_at = text.index("{", start)
    depth = 0
    for i in range(open_at, len(text)):
        depth += text[i] == "{"
        depth -= text[i] == "}"
        if depth == 0:
            return text[open_at:i + 1]
    raise AssertionError("unbalanced braces after " + signature)


class PlayerAuditTests(unittest.TestCase):
    def test_draw_has_no_direct_timer_read(self):
        # Every clock read of Draw() goes through the DP() stage marks, which are constant-false in the player build.
        body = function_body(BACKEND.read_text(), "void Draw(const CmdBuf& buf, const DrawRec& r)")
        stripped = re.sub(r"//[^\n]*", "", body)
        stripped = re.sub(r"\bDP\(\d\);", "", stripped)
        guarded = [line for line in stripped.splitlines() if CLOCK_CALL.search(line) and "kBackendDiagnostics" not in line]
        self.assertEqual(guarded, [], "Draw() reads a clock outside the diagnostics guard")

    def test_draw_stage_marks_compile_away_with_diagnostics_off(self):
        text = BACKEND.read_text()
        self.assertRegex(text, r"#define DP\(i\) do \{ if \(kBackendDiagnostics && g_dprofOn\)")
        self.assertIn("if (kBackendDiagnostics && g_dprofOn) g_dprofT = DNow();", text)

    def test_front_end_files_read_no_clock(self):
        for name in ("vgx_draw.cpp", "vgx_state.cpp", "vgx_dlcache.cpp", "vgx_texcache.cpp", "vgx_bpsync.cpp", "vgx_stats.cpp",
                     "vgx_texdecode.cpp", "vgx_shadergen.cpp"):
            text = re.sub(r"//[^\n]*", "", (VGX_SRC / name).read_text())
            # (log-only timers, never per draw: the shadow-fill summary once a picture, the upload-format log and the decode timer
            # of a texture upload for the stutter log)
            for allowed_name, signature in (("vgx_draw.cpp", "void SfLog()"), ("vgx_texcache.cpp", "inline void TexFormatLogMaybe()"),
                                            ("vgx_texcache.cpp", "struct DecodeTimer")):
                if name == allowed_name and signature in text:
                    text = text.replace(function_body(text, signature), "{}")
            self.assertIsNone(CLOCK_CALL.search(text), f"{name} reads a clock")

    def test_link_requires_pthread_cancel(self):
        cmake = (ROOT / "vita/vgx-game/CMakeLists.txt").read_text()
        self.assertIn("-Wl,--require-defined=pthread_cancel", cmake)
        self.assertRegex(cmake, r"\n\s+-pthread\n")
        code = "\n".join(line for line in cmake.splitlines() if not line.lstrip().startswith("#"))
        self.assertNotIn("-lpthread", code)  # (-pthread makes the compiler driver link libpthread whole)

    def test_entry_logs_libstdcxx_locking(self):
        text = (ROOT / "vita/vgx-game/entry.cpp").read_text()
        self.assertIn("__gthread_active_p()", text)
        self.assertIn("#include <bits/gthr.h>", text)

    def test_vblank_priority_is_opt_in(self):
        text = (ROOT / "platform/vgx/vita/vgx_host_galaxy_vita.cpp").read_text()
        body = function_body(text, "void* VblankMain(void*)")
        self.assertRegex(body, r'static const bool raise = std::getenv\("GXV_VBLANK_PRIO"\) && std::atoi\(std::getenv\("GXV_VBLANK_PRIO"\)\) == 1;')
        self.assertEqual(body.count("port_vita_thread_class(1)"), 1)
        self.assertIn("if (raise && port_vita_thread_class)", body)

    @unittest.skipUnless(os.environ.get("VGX_ELF"), "set VGX_ELF to a linked galaxy_native to check its symbols")
    def test_linked_elf_has_pthread_cancel(self):
        nm = shutil.which("arm-vita-eabi-nm") or os.path.join(os.environ.get("VITASDK", "/opt/vitasdk"), "bin/arm-vita-eabi-nm")
        out = subprocess.run([nm, os.environ["VGX_ELF"]], capture_output=True, text=True, check=True).stdout
        self.assertRegex(out, r" [Tt] pthread_cancel\n")
        self.assertRegex(out, r" [Tt] pthread_key_create\n")


if __name__ == "__main__":
    unittest.main()
