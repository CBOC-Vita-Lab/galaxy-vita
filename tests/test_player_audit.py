"""Source-level guards for the player-build audit: no timer reads per draw, the pthread link guard, the frame-clock switch."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BACKEND = ROOT / "platform/vgx/src/vgx_backend_gl.cpp"
VGX_SRC = ROOT / "platform/vgx/src"
PLAYER_MORPH = ROOT / "decomp/src/Game/Player/MarioActorMorph.cpp"
PLAYER_DRAW = ROOT / "decomp/src/Game/Player/MarioActorDraw.cpp"
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

    def test_bee_morph_retries_model_init_before_transition(self):
        body = function_body(PLAYER_MORPH.read_text(), "void MarioActor::setPlayerMode(u16 playerMode, bool myBool)")
        guard = body[:body.index("if (mPlayerMode == playerMode)")]
        self.assertIn("playerMode == PlayerMode_Bee", guard)
        self.assertIn("initBeeMario();", guard)
        self.assertIn("if (_9E4 == nullptr || _9E8 == nullptr)", guard)
        self.assertIn("return;", guard)

    def test_late_bee_model_gets_texture_swap_tables(self):
        text = PLAYER_DRAW.read_text()
        init = function_body(text, "void MarioActor::initBeeMario()")
        self.assertIn("if (_9E4 != nullptr || _9E8 != nullptr)", init)
        self.assertIn("if (_B64 != nullptr && _B6C != nullptr)", init)
        self.assertIn("initBeeTextureSwap();", init)
        swap = function_body(text, "void MarioActor::swapTextureInit()")
        self.assertIn("initBeeTextureSwap();", swap)
        bee_swap = function_body(text, "void MarioActor::initBeeTextureSwap()")
        self.assertIn("if (_9E4 == nullptr || _B64 == nullptr || _B6C == nullptr)", bee_swap)

    def test_model_switch_invalidates_deferred_mario_materials(self):
        body = function_body(PLAYER_DRAW.read_text(), "void MarioActor::changeDisplayMode(u8 mode)")
        switch = body.index("mModelManager->mDisplayListMaker->mModel = mModels[mCurrModel];")
        invalidate = body.index("mModelManager->mStaleDL = false;")
        self.assertLess(invalidate, switch)
        self.assertIn("#ifdef TARGET_PC", body[:invalidate])
        self.assertIn("if (mModelManager->mDisplayListMaker->mModel != mModels[mode])", body[:invalidate])

        actor = (ROOT / "decomp/src/Game/Player/MarioActor.cpp").read_text()
        calc = function_body(actor, "void MarioActor::calcAnim()")
        self.assertIn("MR::updateModelDiffDL(_9E4);", calc)
        self.assertIn("MR::updateModelDiffDL(this);", calc)

    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler required for the model switch harness")
    def test_deferred_material_queue_across_model_switches(self):
        body = function_body(PLAYER_DRAW.read_text(), "void MarioActor::changeDisplayMode(u8 mode)")
        harness = r'''
#include <cassert>
#include <vector>
using u8 = unsigned char;
using MtxPtr = void*;
struct Model {};
struct XanimePlayer { Model* model = nullptr; void setModel(Model* p) { model = p; } };
struct MarioAnimator { XanimePlayer* mXanimePlayer; XanimePlayer* mXanimePlayerUpper; };
struct DisplayListMaker { Model* mModel; };
struct ModelManager { DisplayListMaker* mDisplayListMaker; bool mStaleDL = false; };
struct Attachment { void setBaseMtx(MtxPtr) {} };
struct MarioActor {
    Model* mModels[3]; u8 mCurrModel; MarioAnimator* mMarioAnim; ModelManager* mModelManager;
    Attachment* _494; Attachment* mSearchLightThrowPos;
    void changeDisplayMode(u8 mode);
};
namespace MR { MtxPtr getJointMtx(MarioActor*, const char*) { return nullptr; } }
void MarioActor::changeDisplayMode(u8 mode)
''' + body + r'''
int main() {
    Model normal, bee;
    XanimePlayer mainAnim, upperAnim;
    MarioAnimator animator{&mainAnim, &upperAnim};
    DisplayListMaker maker{&normal}; ModelManager manager{&maker}; Attachment search;
    MarioActor mario{{&normal, nullptr, &bee}, 0, &animator, &manager, nullptr, &search};
    std::vector<ModelManager*> pending;
    std::vector<Model*> refreshed;
    auto queue = [&] {
        if (!manager.mStaleDL) { manager.mStaleDL = true; pending.push_back(&manager); }
    };
    auto flush = [&] {
        for (auto* m : pending) {
            if (m->mStaleDL) refreshed.push_back(m->mDisplayListMaker->mModel);
            m->mStaleDL = false;
        }
        pending.clear();
    };
    queue(); mario.changeDisplayMode(2); flush();
    assert(refreshed.empty());
    assert(maker.mModel == &bee && mainAnim.model == &bee && upperAnim.model == &bee);
    queue(); mario.changeDisplayMode(2); flush();
    assert(refreshed.size() == 1 && refreshed.back() == &bee);
    queue(); mario.changeDisplayMode(0); queue(); flush();
    assert(refreshed.size() == 2 && refreshed.back() == &normal);
    assert(maker.mModel == &normal && !manager.mStaleDL);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            binary = str(Path(tmp) / "model-switch")
            compiled = subprocess.run([shutil.which("c++"), "-std=c++17", "-DTARGET_PC", "-x", "c++", "-", "-o", binary],
                                      input=harness, text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            executed = subprocess.run([binary], capture_output=True, text=True)
            self.assertEqual(executed.returncode, 0, executed.stderr)

    @unittest.skipUnless(os.environ.get("VGX_ELF"), "set VGX_ELF to a linked galaxy_native to check its symbols")
    def test_linked_elf_has_pthread_cancel(self):
        nm = shutil.which("arm-vita-eabi-nm") or os.path.join(os.environ.get("VITASDK", "/opt/vitasdk"), "bin/arm-vita-eabi-nm")
        out = subprocess.run([nm, os.environ["VGX_ELF"]], capture_output=True, text=True, check=True).stdout
        self.assertRegex(out, r" [Tt] pthread_cancel\n")
        self.assertRegex(out, r" [Tt] pthread_key_create\n")


if __name__ == "__main__":
    unittest.main()
