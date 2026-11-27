// keepawake33 (ported from Strikers 2.0 int33): pure OLED-safe VitaOS keep-awake policy.
//
// VitaOS silently auto-suspends foreground applications after ~15.2-15.5 s without a real
// controller press or sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT), leaving zero crash
// dumps. Keeping the console awake during cold boot, loading-screen unpack/warm-up, active
// Cg shader compilation/linking (sync or async), movies, and unattended test/script runs
// prevents those silent suspends — while ALWAYS allowing normal OLED dimming and standby
// on fatal error screens, while idle in Settings, or during normal idle gameplay/menus.
//
// Header-only and platform-free so tests/keep_awake_test.cpp can exhaustively verify the
// truth table under ASan+UBSan on the host.
#pragma once

#include <cstdint>

namespace port {
namespace keepawake {

struct State {
    bool fatalShown = false;       // port_vgx_vita_error_shown() != 0 (never hold OLED awake on fatal screen)
    bool settingsOpen = false;     // Settings UI active (always allow normal OLED dim/sleep)
    bool keepAwakeFlag = false;    // GXV_KEEP_AWAKE=1, or unset with PETARI_BOOT / PETARI_MOVIE
    bool scriptLive = false;       // active input script currently driving the pad
    bool inMovie = false;          // GXV_MOVIE_AWAKE=1 and THPVideoDecode ran within the last 2 s
    bool compileAwake = true;      // GXV_COMPILE_AWAKE (default 1; 0 disables boot/compile keep-awake)
    bool bootLoading = false;      // boot loading screen actively loading (kDirect or kGl, never kYielded/kDone)
    bool bootStalled = false;      // boot screen entered GXV_BOOT_STALL_SEC hung-boot alert (stop BootOrCompile ticks)
    uint64_t vgxFrames = 0;        // rendered frames so far (< 2 means first title pictures still arriving)
    bool shaderCompiling = false;  // port_vgx_shader_compiling() != 0 (sync Compile/Link or async worker)
};

enum class Reason : uint8_t {
    None = 0,
    KeepAwakeFlag,
    Script,
    Movie,
    BootOrCompile,
};

inline Reason TickReason(const State& s) {
    if (s.fatalShown || s.settingsOpen) return Reason::None;
    if (s.keepAwakeFlag) return Reason::KeepAwakeFlag;
    if (s.scriptLive) return Reason::Script;
    if (s.inMovie) return Reason::Movie;
    if (s.compileAwake && !s.bootStalled && (s.bootLoading || s.vgxFrames < 2u || s.shaderCompiling)) return Reason::BootOrCompile;
    return Reason::None;
}

inline bool ShouldTick(const State& s) { return TickReason(s) != Reason::None; }

inline bool ShouldSplashTick(bool compileAwake, bool fatalShown, bool activeLoadingPhase) {
    return compileAwake && !fatalShown && activeLoadingPhase;
}

}  // namespace keepawake
}  // namespace port
