#include "settings_runtime.h"
#include "../runtime/input.h"
#include "../runtime/audio.h"
#include "port/heap_routing.h"
#include "port/port.h"
#include "port/settings_lifecycle.h"
#include "port/settings_launch.h"
#include "vgx_common.h"

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int OSDisableInterrupts(void);
extern "C" int OSRestoreInterrupts(int);
extern "C" int port_boot_title_is_visible(void);
extern "C" int port_vgx_vita_splash_active(void);
extern "C" int port_vgx_settings_hold(unsigned generation, unsigned timeoutMs);
extern "C" void port_vgx_settings_release(unsigned generation);
extern "C" int port_vgx_vita_fixed_cap_supported(void);
extern "C" void port_input_settings_rearm(void);
extern "C" void port_log_summary(const char*, ...);
extern "C" const char* port_dvd_root(void);

namespace {
using namespace port::settings;
constexpr const char* kPath = "ux0:data/smg/player-settings.cfg";
constexpr size_t kPixels = 960 * 544, kFrameBytes = kPixels * sizeof(uint32_t);
std::atomic<bool> sRequested{false}, sActive{false};
HoldGate sInputHold;
Document sDocument;
Config sActiveConfig;
std::vector<Override> sOverrides;
std::string sStatus;
uint64_t sRetryAfter = 0;

bool Enabled() {
    // GXV_SETTINGS=1 enables the native launch/running menu; default off for A/B.
    static const bool enabled = vgx::EnvInt("GXV_SETTINGS", 0) != 0;
    return enabled;
}
uint64_t Now() { return sceKernelGetProcessTimeWide(); }
int IoError(int value) {
    if (value >= 0) return value;
    const unsigned code = static_cast<unsigned>(value);
    return (code & 0xffff0000u) == 0x80010000u ? -int(code & 0xffffu) : -EIO;
}
int64_t OpenRead(void*, const char* path) { return IoError(sceIoOpen(path, SCE_O_RDONLY, 0)); }
int64_t OpenWrite(void*, const char* path) { return IoError(sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666)); }
int64_t Read(void*, int64_t h, void* p, size_t n) { return IoError(sceIoRead(int(h), p, n)); }
int64_t Write(void*, int64_t h, const void* p, size_t n) { return IoError(sceIoWrite(int(h), p, n)); }
int Sync(void*, int64_t h) { return IoError(sceIoSyncByFd(int(h), 0)); }
int Close(void*, int64_t h) { return IoError(sceIoClose(int(h))); }
int Stat(void*, const char* path, FileInfo& info) {
    SceIoStat st{};
    const int result = IoError(sceIoGetstat(path, &st));
    if (result >= 0) {
        if (st.st_size < 0) return -EIO;
        info = {static_cast<uint64_t>(st.st_size), SCE_S_ISREG(st.st_mode) != 0};
    }
    return result;
}
int Rename(void*, const char* a, const char* b) { return IoError(sceIoRename(a,b)); }
int Remove(void*, const char* path) { return IoError(sceIoRemove(path)); }
const FileOps kFiles{nullptr, OpenRead, OpenWrite, Read, Write, Sync, Close, Stat, Rename, Remove};

bool SameName(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c; };
        if (lower(*a) != lower(*b)) return false;
        if (!*a) return true;
    }
}
struct FontPath {
    char bytes[1024]{};
    bool Join(const char* parent, const char* name) {
        const int length = std::snprintf(bytes, sizeof(bytes), "%s/%s", parent, name);
        return length >= 0 && static_cast<size_t>(length) < sizeof(bytes);
    }
};
template<class Visit> bool ListFontDirectory(const char* path, unsigned& remaining, Visit visit) {
    const int directory = sceIoDopen(path);
    if (directory < 0) return false;
    bool complete = false;
    while (remaining) {
        --remaining;
        SceIoDirent entry{};
        const int result = sceIoDread(directory, &entry);
        if (result <= 0) { complete = result == 0; break; }
        const size_t length = strnlen(entry.d_name, sizeof(entry.d_name));
        if (!length || length == sizeof(entry.d_name) || std::strchr(entry.d_name, '/') || std::strchr(entry.d_name, '\\')) continue;
        if (!std::strcmp(entry.d_name, ".") || !std::strcmp(entry.d_name, "..")) continue;
        visit(entry);
    }
    return sceIoDclose(directory) >= 0 && complete;
}
bool FontDirectory(const char* parent, const char* name, unsigned& remaining, FontPath& out) {
    SceIoStat stat{};
    if (!out.Join(parent, name)) return false;
    if (sceIoGetstat(out.bytes, &stat) >= 0 && SCE_S_ISDIR(stat.st_mode)) return true;
    unsigned matches = 0;
    bool fits = true;
    if (!ListFontDirectory(parent, remaining, [&](const SceIoDirent& entry) {
        if (SCE_S_ISDIR(entry.d_stat.st_mode) && SameName(entry.d_name, name)) { fits = out.Join(parent, entry.d_name); ++matches; }
    })) return false;
    return fits && matches == 1;
}
bool IsFontBucket(const char* name) {
    const size_t length = std::strlen(name);
    if (length < 5 || length > 7 || name[0] != '_' || (name[1] != 'b' && name[1] != 'B') || (name[2] != 'k' && name[2] != 'K')) return false;
    for (size_t i = 3; i < length; ++i) if (name[i] < '0' || name[i] > '9') return false;
    return true;
}
bool FontArchivePath(FontPath& out) {
    const char* root = port_dvd_root();
    if (!root || !*root || std::strlen(root) > 512) return false;
    // Resolve only LayoutData and its bucket children. This never initializes
    // the DVD/FST workers, migrates files, or scans unrelated game archives.
    unsigned remaining = 4096;
    FontPath files, layout;
    if (!FontDirectory(root, "files", remaining, files) || !FontDirectory(files.bytes, "LayoutData", remaining, layout)) return false;
    FileInfo info{};
    if (!out.Join(layout.bytes, "Font.arc")) return false;
    if (Stat(nullptr, out.bytes, info) >= 0 && info.regular) return true;
    char buckets[128][8]{};
    unsigned matches = 0, bucketCount = 0;
    bool fits = true;
    auto candidate = [&](const char* directory, const SceIoDirent& entry) {
        if (SCE_S_ISREG(entry.d_stat.st_mode) && SameName(entry.d_name, "Font.arc")) { fits = out.Join(directory, entry.d_name) && fits; ++matches; }
    };
    if (!ListFontDirectory(layout.bytes, remaining, [&](const SceIoDirent& entry) {
        candidate(layout.bytes, entry);
        if (SCE_S_ISDIR(entry.d_stat.st_mode) && IsFontBucket(entry.d_name)) {
            if (bucketCount == 128) fits = false;
            else std::strcpy(buckets[bucketCount++], entry.d_name);
        }
    })) return false;
    if (!fits) return false;
    for (unsigned i = 0; i < bucketCount; ++i) {
        FontPath bucket;
        if (!bucket.Join(layout.bytes, buckets[i]) || !ListFontDirectory(bucket.bytes, remaining, [&](const SceIoDirent& entry) { candidate(bucket.bytes, entry); })) return false;
    }
    return fits && matches == 1;
}
FontBundle LoadMenuFonts(const FontAllocator& allocator = {}) {
    FontBundle fonts;
    FontPath path;
    FileInfo info{};
    // Bound the transient native read separately from the parser's decoded
    // limits; oversized/unsupported installed fonts keep the built-in fallback.
    constexpr size_t nativeArchiveLimit = 8 * 1024 * 1024;
    if (!FontArchivePath(path) || Stat(nullptr, path.bytes, info) < 0 || !info.regular || !info.size || info.size > nativeArchiveLimit) return fonts;
    const size_t length = static_cast<size_t>(info.size);
    FontBuffer<uint8_t> bytes;
    if (!bytes.Allocate(length, allocator)) return fonts;
    const int64_t handle = OpenRead(nullptr, path.bytes);
    if (handle < 0) return fonts;
    size_t offset = 0;
    while (offset < length) {
        const int64_t count = Read(nullptr, handle, bytes.begin() + offset, length - offset);
        if (count <= 0 || static_cast<uint64_t>(count) > length - offset) break;
        offset += static_cast<size_t>(count);
    }
    const int closed = Close(nullptr, handle);
    const char* error = nullptr;
    if (offset != length || closed < 0 || !ParseGameFontArchive(bytes.begin(), length, fonts, error, allocator)) return {};
    return fonts;
}

struct Screen {
    SceUID memory = -1;
    void* base = nullptr;
    uint32_t* pixels = nullptr;
    SceDisplayFrameBuf previous{};
    unsigned next = 0;
    bool published = false, pendingFrame = false;
    int inputSource = -2;
    unsigned controllerType = 0;
    bool Acquire(bool running) {
        previous.size = sizeof(previous);
        const int got = sceDisplayGetFrameBuf(&previous, SCE_DISPLAY_SETBUF_NEXTFRAME);
        if (got < 0) previous = {};
        if (running && !previous.base) return false;
        pixels = static_cast<uint32_t*>(std::calloc(kPixels, sizeof(uint32_t)));
        if (!pixels) return false;
        const size_t bytes = (kFrameBytes * 2 + 262143) & ~size_t(262143);
        memory = sceKernelAllocMemBlock("galaxy_settings", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, bytes, nullptr);
        if (memory < 0 || sceKernelGetMemBlockBase(memory, &base) < 0 || !base) {
            if (memory >= 0) sceKernelFreeMemBlock(memory);
            memory = -1;
            std::free(pixels); pixels = nullptr;
            return false;
        }
        return true;
    }
    bool Present(const Surface& surface) {
        if (surface.width != 960 || surface.height != 544 || surface.stride < 960 || !surface.pixels) return false;
        // A failed wait leaves display ownership unknown. Do not overwrite a
        // possibly scanned-out buffer until a later vblank confirms retirement.
        if (pendingFrame && sceDisplayWaitVblankStart() < 0) return false;
        pendingFrame = false;
        auto* buffer = static_cast<uint8_t*>(base) + next * kFrameBytes;
        for (unsigned y = 0; y < 544; ++y)
            std::memcpy(buffer + y * 960 * 4, surface.pixels + y * surface.stride, 960 * 4);
        SceDisplayFrameBuf frame{};
        frame.size = sizeof(frame); frame.base = buffer; frame.pitch = 960;
        frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8; frame.width = 960; frame.height = 544;
        if (sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0) return false;
        published = pendingFrame = true;
        next ^= 1;
        if (sceDisplayWaitVblankStart() < 0) return false;
        pendingFrame = false;
        return true;
    }
    bool Release() {
        if (published) {
            if (sceDisplaySetFrameBuf(previous.base ? &previous : nullptr, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0) return false;
            pendingFrame = true;
            if (sceDisplayWaitVblankStart() < 0) return false;
        }
        published = pendingFrame = false;
        if (memory >= 0) sceKernelFreeMemBlock(memory);
        memory = -1; base = nullptr;
        std::free(pixels); pixels = nullptr;
        return true;
    }
};
bool Poll(void* context, UiInput& input) {
    auto& screen = *static_cast<Screen*>(context);
    PortVitaRawInput raw;
    port_vita_input_raw_poll(raw);
    input = {};
    const bool changed = screen.inputSource != -2 && (screen.inputSource != raw.source || screen.controllerType != raw.controllerType);
    screen.inputSource = raw.source; screen.controllerType = raw.controllerType;
    if (changed) return true; // One disconnected sample rearms UI after source changes.
    input.caps = raw.caps; input.physicalHeld = raw.held;
    if (raw.source >= 0 && raw.caps.connected)
        input.detectedProfile = raw.controllerType == SCE_CTRL_TYPE_PHY ? port::controls::ProfileId::Handheld : port::controls::ProfileId::PlayStation;
    using namespace port::controls;
    if ((raw.held & Button::Up) || raw.leftY > 0.6f) input.menuHeld |= MenuKey::Up;
    if ((raw.held & Button::Down) || raw.leftY < -0.6f) input.menuHeld |= MenuKey::Down;
    if ((raw.held & Button::Left) || raw.leftX < -0.6f) input.menuHeld |= MenuKey::Left;
    if ((raw.held & Button::Right) || raw.leftX > 0.6f) input.menuHeld |= MenuKey::Right;
    if (raw.held & Cross) input.menuHeld |= Accept;
    if (raw.held & Circle) input.menuHeld |= Back;
    if (raw.held & Triangle) input.menuHeld |= Reset;
    if (raw.held & L1) input.menuHeld |= PreviousPage;
    if (raw.held & R1) input.menuHeld |= NextPage;
    return true;
}
bool Present(void* context, const Surface& surface) { return static_cast<Screen*>(context)->Present(surface); }
uint64_t HostNow(void*) { return Now(); }
void Wait(void*, uint64_t until) { const auto now = Now(); if (until > now) sceKernelDelayThread(unsigned(std::min<uint64_t>(until-now, 100000))); }
RunResult Menu(Screen& screen, OpenMode mode) {
    PortHostAllocScope allocations;
    const auto& api = port::controls::GetControlApi();
    const auto loaded = LoadDocument(kPath, api, kFiles, sDocument);
    UiInput initial{}; Poll(&screen, initial);
    SettingsSession session{mode, sDocument, sActiveConfig, ReadLanguages(), sOverrides, initial.caps,
        {port_vgx_vita_fixed_cap_supported() != 0, port::controls::RemapEnabled()},
        {screen.pixels, 960, 544, 960}, {&screen, Poll, Present, HostNow, Wait}, kPath, &kFiles, &api};
    session.statusMessage = sStatus.empty() ? loaded.message : sStatus;
    session.detectedProfile = initial.detectedProfile;
    const auto fonts = LoadMenuFonts();
    session.fonts = fonts.view();
    if (mode == OpenMode::Running && port::controls::RemapEnabled() && !port_vita_input_mapping_available())
        session.overrides.push_back({"Controller", "", "Active mapping needs a different controller"});
    sStatus.clear();
    return Run(session);
}
RunResult MenuAndRetire(Screen& screen, OpenMode mode) {
    for (;;) {
        const auto result = Menu(screen, mode);
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            if (screen.Release()) return result;
            sceKernelDelayThread(100000);
        }
        // Keep every gameplay hold and all UI storage until scanout restoration
        // is confirmed. A device failure must not resume unseen gameplay.
        sStatus = "Display could not be restored. Please try again.";
        port_log_summary("settings: display restore failed; gameplay remains held for retry");
    }
}
LaunchRequest TakeLaunch(int argc = 0, char** argv = nullptr) {
    char parameter[1024] = {};
    const int rc = sceAppMgrGetAppParam(parameter);
    // The system may keep returning the same LiveArea parameter after it was read. Act on a
    // Settings parameter once: the same text again (the next poll, or after a -play restart) is
    // not a new request until a different parameter has been seen in between.
    static char consumed[1024] = {};
    const LaunchRequest fromParameter = rc >= 0 ? ParseLaunch(0, nullptr, parameter, sizeof(parameter)) : LaunchRequest::Play;
    const bool repeat = rc >= 0 && consumed[0] && std::strncmp(consumed, parameter, sizeof(consumed)) == 0;
    if (rc < 0 || fromParameter != LaunchRequest::Settings) consumed[0] = 0;
    else std::memcpy(consumed, parameter, sizeof(consumed));
    if (repeat && !argc) return LaunchRequest::Play;
    return ParseLaunch(argc, argv, rc >= 0 ? parameter : nullptr, rc >= 0 ? sizeof(parameter) : 0);
}
bool Relaunch() {
    char play[] = "-play";
    char* args[] = {play, nullptr};
    const int rc = sceAppMgrLoadExec("app0:eboot.bin", args, nullptr);
    // A successful LoadExec never returns to this process.
    port_log_summary("settings: restart returned 0x%08X; saved choices remain next-launch only", unsigned(rc));
    sStatus = "Saved for next launch. Restart failed.";
    return false;
}
struct RuntimeLease {
    Screen screen;
    bool uiOwned = false;
    unsigned generation = 0;
    int interrupts = 0, clock = 0, paused = 0, audio = 0;
    static bool Acquire(void* p, Resource resource, unsigned timeoutMs) {
        auto& self = *static_cast<RuntimeLease*>(p);
        switch (resource) {
        case Resource::Input: {
            self.generation = sInputHold.Begin();
            const auto deadline = Now() + uint64_t(timeoutMs) * 1000;
            while (self.generation && !sInputHold.Acknowledged(self.generation) && Now() < deadline) sceKernelDelayThread(1000);
            if (sInputHold.Acknowledged(self.generation)) return true;
            sInputHold.Cancel(self.generation); return false;
        }
        case Resource::Interrupts: self.interrupts = OSDisableInterrupts(); return true;
        case Resource::Clock: self.clock = port_settings_time_pause(1); return true;
        case Resource::Producers: self.paused = port_is_paused(); port_set_paused(1); return true;
        case Resource::Audio: self.audio = port_vita_audio_is_held(); return port_vita_audio_hold(1, timeoutMs) != 0;
        case Resource::Renderer: return port_vgx_settings_hold(self.generation, timeoutMs) != 0;
        case Resource::Scanout: return self.screen.Acquire(true);
        default: return false;
        }
    }
    static void Release(void* p, Resource resource) {
        auto& self = *static_cast<RuntimeLease*>(p);
        switch (resource) {
        case Resource::Scanout: self.screen.Release(); break;
        case Resource::Renderer: port_vgx_settings_release(self.generation); break;
        case Resource::Audio: port_vita_audio_hold(self.audio, 250); break;
        case Resource::Producers:
            // Clear game latches while interrupts and the input publisher are
            // still held; restoring interrupts can immediately run a callback.
            if (self.uiOwned) { port_input_settings_rearm(); port_vita_input_settings_rearm(); }
            port_set_paused(self.paused); break;
        case Resource::Clock: port_settings_time_pause(self.clock); break;
        case Resource::Interrupts: OSRestoreInterrupts(self.interrupts); break;
        case Resource::Input: sInputHold.Cancel(self.generation); break;
        default: break;
        }
    }
};
} // namespace

extern "C" int port_settings_startup(int argc, char** argv) {
    if (!Enabled()) return 0;
    const auto request = TakeLaunch(argc, argv); // Consume even a stale parameter overridden by -play.
    const auto& api = port::controls::GetControlApi();
    LoadDocument(kPath, api, kFiles, sDocument);
    sActiveConfig = sDocument.config;
    sOverrides = ApplyStartup(sActiveConfig, true);
    std::string error;
    const unsigned profile = sActiveConfig.profile == port::controls::ProfileId::Handheld ? 0 : 1;
    if (!port::controls::SetActiveMapping(sActiveConfig.profiles[profile], sActiveConfig.profile, port::controls::NominalCaps(sActiveConfig.profile), error))
        port_log_summary("settings: active mapping rejected: %s", error.c_str());
    if (request != LaunchRequest::Settings) return 0;
    port_dvd_set_root("ux0:data/smg/game");
    if (!port_vita_input_raw_init()) return 1;
    for (;;) {
        Screen screen;
        if (!screen.Acquire(false)) { port_log_summary("settings: screen allocation failed"); return 1; }
        const auto result = MenuAndRetire(screen, OpenMode::ColdStart);
        if (result.intent != ExitIntent::Play) return 1;
        Relaunch();
    }
}
extern "C" void port_settings_request(void) { if (Enabled()) sRequested.store(true, std::memory_order_release); }
extern "C" int port_settings_active(void) { return sActive.load(std::memory_order_acquire) ? 1 : 0; }
extern "C" int port_settings_input_service(void) {
    if (!Enabled()) return 0;
    const unsigned generation = sInputHold.Requested();
    if (generation && sInputHold.Acknowledge(generation)) {
        while (sInputHold.Requested() == generation) sceKernelDelayThread(1000);
        return 1;
    }
    static uint64_t pollAfter = 0;
    if (Now() >= pollAfter) {
        pollAfter = Now() + 250000;
        if (TakeLaunch() == LaunchRequest::Settings) port_settings_request();
    }
    return 0;
}
extern "C" void port_settings_game_boundary(int safe) {
    if (!Enabled() || !safe || !sRequested.load(std::memory_order_acquire) || !port_boot_title_is_visible() || port_vgx_vita_splash_active() || Now() < sRetryAfter) return;
    PortHostAllocScope allocations;
    RuntimeLease context;
    Hooks hooks{&context, RuntimeLease::Acquire, RuntimeLease::Release};
    Lease lease;
    sActive.store(true, std::memory_order_release);
    static unsigned failedOpens = 0;
    if (!lease.Acquire(hooks, 250)) {
        sActive.store(false, std::memory_order_release);
        sRetryAfter = Now() + 1000000;
        // Each attempt briefly holds input, audio and the renderer: give up after three in a row
        // (for example no memory for the menu screen) instead of stuttering once a second.
        if (++failedOpens >= 3) {
            failedOpens = 0;
            sRequested.store(false, std::memory_order_release);
            port_log_summary("settings: the menu could not open (3 attempts); request dropped");
        }
        return;
    }
    failedOpens = 0;
    sRequested.store(false, std::memory_order_release);
    const uint64_t opened = Now();
    port_log_summary("settings: gameplay held; menu time is not gameplay performance");
    context.uiOwned = true;
    const auto result = MenuAndRetire(context.screen, OpenMode::Running);
    lease.Release(hooks);
    sActive.store(false, std::memory_order_release);
    port_log_summary("settings: gameplay resumed after %.2f s; exclude the crossing FPS window", (Now()-opened)/1000000.0);
    if (result.intent == ExitIntent::Restart) {
        Relaunch();
        sRequested.store(true, std::memory_order_release);
    }
}
