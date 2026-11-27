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

    # Clock reads that sit behind a switch or a log gate (tests31: the audit used to stop at the first of them). Each entry is
    # (file, source anchor, gate): the anchor's body is removed from the text that is searched for clock reads, and the gate
    # text must itself be in that body (so the exemption goes when someone removes the gate).
    GATED_BODIES = (
        ("vgx_state.cpp", "auto runClose = [&]() {", "if (runTimed)"),                 # FE split-run timer: runTimed = FeTimedScope()
        ("vgx_dlcache.cpp", "if (census && port_host_time_ns) {", "g_census"),    # the bypass census
        ("vgx_dlcache.cpp", "if (ewOn) {", "g_dlBuildNs"),                            # GXV_ENDWAIT_LOG
        ("vgx_texcache.cpp", "void RecordId(const TexObj& o, uint32_t fmt, uint32_t levels, uint32_t size, uint64_t hash) {", "InWindow"),
        ("vgx_texcache.cpp", "void Record(const TexObj& o, uint32_t fmt, uint32_t levels, uint32_t size, uint64_t hash) {", "if (Mode() == 0"),
        ("vgx_texcache.cpp", "static int ReplayUpload(const void* p,", "g_inReplay"),   # texwarm replay (GXV_TEXWARM only)
        ("vgx_texcache.cpp", "static texid::Outcome TryId(", "g_ci"),                   # texwarm key mode only
        ("vgx_texcache.cpp", "static void StepId() {", "IdSave"),                       # texwarm key mode only (Step returns on Mode() == 0)
        ("vgx_texcache.cpp", "static void Step() {", "if (Mode() == 0) return;"),
        ("vgx_texcache.cpp", "void AmFrameEnd() {", "if (c.mode != 1) return;"),      # GXV_AUTOMIP
        ("vgx_texcache.cpp", "if (popinTex && port_log_summary", "port_log_summary"),          # GXV_POPIN_LOG
    )
    # One clock read a picture (never per draw), for a log line's interval: allowed like the FE log timer.
    PER_PICTURE_BODIES = (("vgx_texcache.cpp", "void Picture() {", "g_pics"),)
    # Startup-only initialisers and a type name (no read in the draw path).
    GATED_LINES = (("vgx_state.cpp", "runT = runTimed ? port_host_time_ns() : 0;"),
                   ("vgx_dlcache.cpp", "const int64_t ewT0 = ewOn ? port_host_time_ns() : 0;"),
                   ("vgx_texcache.cpp", "std::chrono::steady_clock::time_point g_popinLastTime;"),
                   ("vgx_texcache.cpp", "std::chrono::steady_clock::time_point g_amLogT = std::chrono::steady_clock::now();"),
                   ("vgx_texcache.cpp", "std::chrono::steady_clock::time_point g_t0 = std::chrono::steady_clock::now();"),
                   ("vgx_texcache.cpp", "double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }"))   # the helper itself; its callers are the gated bodies above

    def strip_switch_gated_clocks(self, name, text):
        for file, anchor, gate in self.GATED_BODIES + self.PER_PICTURE_BODIES:
            if file != name or anchor not in text:
                continue
            body = function_body(text, anchor)
            self.assertIn(gate, body, f"{name}: the gate '{gate}' of '{anchor}' is gone, so its clock reads are no longer gated")
            text = text.replace(body, "{}", 1)
        for file, line in self.GATED_LINES:
            if file == name:
                text = text.replace(line, "")
        return text

    def test_front_end_files_read_no_clock(self):
        for name in ("vgx_draw.cpp", "vgx_state.cpp", "vgx_dlcache.cpp", "vgx_texcache.cpp", "vgx_bpsync.cpp", "vgx_stats.cpp",
                     "vgx_texdecode.cpp", "vgx_shadergen.cpp"):
            text = re.sub(r"//[^\n]*", "", (VGX_SRC / name).read_text())
            # (log-only timers, never per draw: the front end's one clock read a picture for its log lines (dma31: FeClock, which also
            # holds the GXV_DYN_CENSUS scope's reads), the upload-format log and the decode timer of a texture upload for the stutter log)
            for allowed_name, signature in (("vgx_draw.cpp", "struct FeClock {"), ("vgx_texcache.cpp", "inline void TexFormatLogMaybe()"),
                                            ("vgx_texcache.cpp", "struct DecodeTimer")):
                if name == allowed_name and signature in text:
                    text = text.replace(function_body(text, signature), "{}")
            text = self.strip_switch_gated_clocks(name, text)
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

    # ---- beefix25 (11 Nov): the Bee power-up path, checked against community PR #34 (Arfatnaik0). Structure of the game source ...
    def test_bee_mode_is_refused_before_any_state_is_touched(self):
        body = function_body(PLAYER_MORPH.read_text(), "void MarioActor::setPlayerMode(u16 playerMode, bool myBool)")
        refuse = body.index("if (playerMode == 4 && (!_9E4 || !_9E8))")
        self.assertLess(refuse, body.index("_3D6 = mPlayerMode;"))
        self.assertLess(refuse, body.index("mPlayerMode = playerMode;"))
        self.assertLess(refuse, body.index("closeStatus(nullptr)"))
        self.assertIn("return;", body[refuse:body.index("#ifdef TARGET_PC", refuse)])
        self.assertIn("if (!canEnterPlayerMode(playerMode))", body[:body.index("_3D6 = mPlayerMode;")])

    def test_bee_retry_rule_is_the_tested_helper(self):
        text = PLAYER_MORPH.read_text()
        can = function_body(text, "bool MarioActor::canEnterPlayerMode(u16 playerMode)")
        self.assertIn("portBeeRetryWanted(", can)
        self.assertIn("portBeeArchiveName(gIsLuigi)", can)
        self.assertIn('"GXV_POWERUP_RETRY"', can)
        self.assertLess(can.index("portBeeRetryWanted("), can.index("initBeeMario();"))
        self.assertLess(can.index("initBeeMario();"), can.index("portPlayerModeMissing(playerMode, assets)"))

    def test_item_is_not_used_up_when_the_form_is_refused(self):
        item = (ROOT / "decomp/src/Game/MapObj/MorphItemObjNeo.cpp").read_text()
        body = function_body(item, "bool MorphItemObjNeo::receiveOtherMsg(u32 msg, HitSensor* pSender, HitSensor* pReceiver)")
        ask = body.index("!MR::canChangePlayerItemStatus(mMorphType)")
        self.assertLess(ask, body.index("MR::startSound(this, \"SE_OJ_ITEM_BUBBLE_BREAK\")"))
        self.assertLess(ask, body.index("MR::changePlayerItemStatus(mMorphType);"))
        self.assertLess(ask, body.index("makeActorDead();"))
        self.assertIn("return false;", body[ask:body.index("MR::startSound", ask)])
        access = (ROOT / "decomp/src/Game/Player/MarioAccess.cpp").read_text()
        self.assertIn("getPlayerActor()->canEnterPlayerMode((u16)mode)", function_body(access, "bool canChangeItemStatus(s32 item)"))
        util = (ROOT / "decomp/src/Game/Util/PlayerUtil.cpp").read_text()
        self.assertIn("MarioAccess::canChangeItemStatus(param1)", function_body(util, "bool canChangePlayerItemStatus(s32 param1)"))

    def test_late_bee_model_gets_texture_slots_with_bounds(self):
        text = PLAYER_DRAW.read_text()
        init = function_body(text, "void MarioActor::initBeeMario()")
        self.assertIn("if (_9E4 != nullptr || _9E8 != nullptr)", init[:init.index("J3DModelX* model;")])
        tail = init[init.index("_3D2 |= 0x10;"):]
        self.assertIn("if (_B64 != nullptr && _B6C != nullptr)", tail)
        self.assertIn("initBeeTextureSwap();", tail)
        self.assertIn('MR::hideJoint(mModels[2], "HandR0");', tail)
        # (function_body cannot be used here: the #ifdef TARGET_PC / #else pair of the hopper block opens a brace twice)
        swap = text[text.index("void MarioActor::swapTextureInit() {"):text.index("void MarioActor::initFace()")]
        self.assertIn("initBeeTextureSwap();", swap)
        self.assertNotIn("_B64[i] = MR::getResTIMG(_9E4, i);", swap)
        bee = function_body(text, "void MarioActor::initBeeTextureSwap()")
        self.assertIn("_9E4 == nullptr || _B64 == nullptr || _B6C == nullptr || _B60 == 0", bee)
        self.assertIn("i < textureNum && i < _B60", bee)
        self.assertIn("portBeeEyelidFits(first, textureNum, _B60)", bee)
        self.assertIn("portBeeEyelidFirst(found, _B70)", bee)
        create = function_body(text, "void MarioActor::createTextureDL(DLholder* pHolder, u16 texMapID, u16 texIndex)")
        self.assertIn("texIndex >= _B60", create[:create.index("MR::ProhibitSchedulerAndInterrupts")])
        self.assertIn("for (u32 i = 0; i < 12; i++)", swap)

    # ... and the real initBeeTextureSwap body compiled on the host with fakes (AddressSanitizer: no read or write past _B64).
    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler required for the Bee texture harness")
    def test_bee_texture_swap_bounds_on_the_real_function(self):
        body = function_body(PLAYER_DRAW.read_text(), "void MarioActor::initBeeTextureSwap()")
        harness = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "port/player_assets.h"
using u8 = unsigned char; using u16 = unsigned short; using u32 = unsigned int;
static int sLogs = 0;
extern "C" void port_log_summary(const char*, ...) { sLogs++; }
struct ResTIMG { int id; };
struct DLholder { u8* mDL; u16 mSize; u16 _6; };
struct NameTab { std::vector<const char*> names; const char* getName(u16 i) const { return names.at(i); } };
struct TexTab { u16 num; u16 getNum() const { return num; } };
struct MatTable { TexTab* mTexture; NameTab* mTextureName; };
struct J3DModelData { MatTable mMaterialTable; };
struct ModelHolder { J3DModelData data; std::vector<ResTIMG> imgs; };
namespace MR {
J3DModelData* getJ3DModelData(ModelHolder* h) { return &h->data; }
ResTIMG* getResTIMG(ModelHolder* h, u32 i) { return &h->imgs.at(i); }
}
struct MarioActor {
    ModelHolder* _9E4; ResTIMG** _B64; DLholder* _B6C; u16 _B60; u16 _B70;
    std::vector<unsigned> made;
    void createTextureDL(DLholder* h, u16, u16 idx) {
        assert(_B64 != nullptr && idx < _B60);  // (the real one makes no list for these; the caller must not ask)
        static u8 dl; h->mDL = &dl; h->mSize = 8; made.push_back(idx);
    }
    void initBeeTextureSwap();
};
void MarioActor::initBeeTextureSwap()
''' + body + r'''
struct Fx {
    TexTab tex; NameTab names; ModelHolder bee; ResTIMG** table; DLholder* slots; MarioActor a;
    Fx(unsigned baseCount, unsigned beeCount, int eyelidAt, unsigned normalEyelid) {
        tex.num = (u16)beeCount;
        for (unsigned i = 0; i < beeCount; i++) names.names.push_back((int)i == eyelidAt ? "mario_eyeLid.0" : "other");
        bee.data.mMaterialTable = {&tex, &names};
        bee.imgs.resize(beeCount);
        table = new ResTIMG*[baseCount];  // exactly the base body's count: AddressSanitizer sees any access past it
        for (unsigned i = 0; i < baseCount; i++) table[i] = nullptr;
        slots = new DLholder[12]();
        a = MarioActor{&bee, table, slots, (u16)baseCount, (u16)normalEyelid, {}};
    }
    ~Fx() { delete[] table; delete[] slots; }
    unsigned nMade() const { int n = 0; for (int i = 4; i < 8; i++) n += slots[i].mDL != nullptr; return n; }
};
int main() {
    { Fx f(12, 12, 8, 4); f.a.initBeeTextureSwap(); assert(f.a.made == (std::vector<unsigned>{8, 9, 10, 11}) && f.nMade() == 4 && sLogs == 0);
      f.a.made.clear(); f.a.initBeeTextureSwap(); assert(f.a.made.size() == 4 && f.nMade() == 4); }  // re-entry: the same again
    { Fx f(12, 12, 0, 4); f.a.initBeeTextureSwap(); assert(f.a.made == (std::vector<unsigned>{0, 1, 2, 3})); }
    // the Bee has MORE textures than the base body: only _B60 are stored, an eyelid run inside them is made, one outside is skipped
    { Fx f(6, 20, 2, 0); f.a.initBeeTextureSwap(); assert(f.a.made == (std::vector<unsigned>{2, 3, 4, 5}) && f.nMade() == 4); }
    { int before = sLogs; Fx f(6, 20, 3, 0); f.a.initBeeTextureSwap(); assert(f.a.made.empty() && f.nMade() == 0 && sLogs == before + 1); }  // community: _B64[6..19] written
    // no "mario_eyeLid.0": the normal body's index (the original code), not 0
    { Fx f(8, 8, -1, 2); f.a.initBeeTextureSwap(); assert(f.a.made == (std::vector<unsigned>{2, 3, 4, 5})); }
    { int before = sLogs; Fx f(8, 8, -1, 6); f.a.initBeeTextureSwap(); assert(f.a.made.empty() && sLogs == before + 1); }  // 6..9 is past the table
    // eyelid found with fewer than four textures after it; a Bee smaller than the base body
    { Fx f(12, 10, 8, 4); f.a.initBeeTextureSwap(); assert(f.a.made.empty() && f.nMade() == 0); }
    { Fx f(12, 5, 1, 4); f.a.initBeeTextureSwap(); assert(f.a.made == (std::vector<unsigned>{1, 2, 3, 4})); }
    { Fx f(12, 3, 0, 4); f.a.initBeeTextureSwap(); assert(f.a.made.empty()); }
    // not initialised yet: nothing happens
    { Fx f(12, 12, 8, 4); f.a._B6C = nullptr; f.a.initBeeTextureSwap(); assert(f.a.made.empty()); }
    { Fx f(12, 12, 8, 4); f.a._B64 = nullptr; f.a.initBeeTextureSwap(); assert(f.a.made.empty()); }
    { Fx f(12, 12, 8, 4); f.a._9E4 = nullptr; f.a.initBeeTextureSwap(); assert(f.a.made.empty()); }
    { Fx f(12, 12, 8, 4); f.a._B60 = 0; f.a.initBeeTextureSwap(); assert(f.a.made.empty()); }
    std::puts("ok");
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            binary = str(Path(tmp) / "bee-texture-swap")
            flags = ["-fsanitize=address"] if os.environ.get("BEE_NO_ASAN") is None else []
            compiled = subprocess.run([shutil.which("c++"), "-std=c++17", "-g", "-I", str(ROOT / "platform/include"), *flags, "-x", "c++", "-", "-o", binary],
                                      input=harness, text=True, capture_output=True)
            if compiled.returncode != 0 and flags:  # (a host without the sanitizer runtime: plain build)
                compiled = subprocess.run([shutil.which("c++"), "-std=c++17", "-g", "-I", str(ROOT / "platform/include"), "-x", "c++", "-", "-o", binary],
                                          input=harness, text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            executed = subprocess.run([binary], capture_output=True, text=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    # ... the Mario / Luigi entry rule as a model around the real helper functions (tests/bee_late_init.cpp)
    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler required for the Bee entry model")
    def test_bee_entry_rule_model_for_mario_and_luigi(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = str(Path(tmp) / "bee-late-init")
            compiled = subprocess.run([shutil.which("c++"), "-std=c++17", "-Wall", "-Wextra", "-I", str(ROOT / "platform/include"),
                                       str(ROOT / "tests/bee_late_init.cpp"), "-o", binary], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            executed = subprocess.run([binary], capture_output=True, text=True)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
            self.assertIn("PASS bee_late_init", executed.stdout)

    def test_bee_late_init_switch_and_default(self):
        entry = (ROOT / "vita/vgx-game/entry.cpp").read_text()
        self.assertIn('setenv("GXV_POWERUP_RETRY", "0", 0);', entry)  # (default 0 until a console run; see agents/beefix25 REPORT)
        self.assertIn('setenv("GXV_DL_LAZY_KEY", "1", 0);', entry)
        self.assertIn('setenv("GXV_DL_GUARD", "1", 0);', entry)

    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler required for the model switch harness")
    def test_deferred_material_refresh_follows_its_model(self):
        # ours (GXV_DL_LAZY_KEY) keys the refresh to the model it was asked for instead of clearing mStaleDL at the switch (PR #34).
        text = (ROOT / "decomp/src/Game/LiveActor/ModelManager.cpp").read_text()
        refresh = function_body(text, "void PortDlRefreshOne(const StaleDL& e, int staticMode, bool lazyKey, DlCounts& c)")
        self.assertIn("e.model->mStaleDL && lazyKey && !PortDlTargetSame(e.model)", refresh)
        self.assertEqual(text.count("PortDlNoteTarget(model);"), 1)
        self.assertEqual(text.count("PortDlNoteTarget(this);"), 1)

    @unittest.skipUnless(os.environ.get("VGX_ELF"), "set VGX_ELF to a linked galaxy_native to check its symbols")
    def test_linked_elf_has_pthread_cancel(self):
        nm = shutil.which("arm-vita-eabi-nm") or os.path.join(os.environ.get("VITASDK", "/opt/vitasdk"), "bin/arm-vita-eabi-nm")
        out = subprocess.run([nm, os.environ["VGX_ELF"]], capture_output=True, text=True, check=True).stdout
        self.assertRegex(out, r" [Tt] pthread_cancel\n")
        self.assertRegex(out, r" [Tt] pthread_key_create\n")


if __name__ == "__main__":
    unittest.main()
