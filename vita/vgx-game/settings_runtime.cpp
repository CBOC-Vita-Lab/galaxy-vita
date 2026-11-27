#include "settings_runtime.h"
#include "../runtime/input.h"
#include "../runtime/audio.h"
#include "port/heap_routing.h"
#include "port/port.h"
#include "port/settings_lifecycle.h"
#include "port/settings_launch.h"
#include "port/settings_guard.h"
#include "port/system_language.h"
#include "port/settings_native.h"  // settingsnative
#ifndef GALAXY_SETTINGS_VERSION
#error "GALAXY_SETTINGS_VERSION is not defined: vita/vgx-game/CMakeLists.txt derives it from SMG_APP_VERSION (platform/src/mp/mp_service.cpp); there is no default on purpose"
#endif
#include "port/settings_backdrop.h"
#include "port/mp_bridge.h"
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
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int OSDisableInterrupts(void);
extern "C" int OSRestoreInterrupts(int);
extern "C" int port_boot_title_is_visible(void);
extern "C" void port_par_plant_close_early(void) __attribute__((weak));  // (GXV_PAR_PLANT_EARLY, parplant23: ModelManager.cpp)
extern "C" int port_vgx_vita_splash_active(void);
extern "C" int port_vgx_settings_hold(unsigned generation, unsigned timeoutMs);
extern "C" void port_vgx_settings_release(unsigned generation);
extern "C" int port_vgx_vita_fixed_cap_supported(void);
extern "C" void port_input_settings_rearm(void);
extern "C" void port_log_summary(const char*, ...) __attribute__((format(printf, 1, 2)));
extern "C" int sceAppMgrReceiveEvent(SceAppMgrEvent*);  // (in the SceAppMgr stub; the SDK header only declares the underscore form)
extern "C" const char* port_dvd_root(void);
// MP3: the multiplayer toast (ui module, decomp/src/Game/MP; game thread). 0x0306 = MP_TXT_NO_SETTINGS (Game/MP/MpFlow.hpp).
// Both are WEAK: this file is also built by the host tests (tests/settings_runtime.cpp, tests/settings_save_guard.cpp) and by builds
// without multiplayer, where neither exists; they are tested for null before every call. (mp_active: same signature as port/mp_bridge.h.)
extern "C" bool mp_active(void) __attribute__((weak));
extern "C" bool mp_port_quit(unsigned budgetMs) __attribute__((weak));  // (merge fix 5: platform/src/mp/mp_boot.cpp)
namespace MpUi { void toast(uint16_t textId, int slot, uint32_t arg) __attribute__((weak)); }
namespace GameSystemFunction { bool isPrepareResetSaveDataHandleSequence(); }

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
// (polish23) a new request (port_settings_request after none was pending) bumps this; the game thread then starts its attempt count
// and give-up clock again
std::atomic<unsigned> sRequestSeq{0};
// (polish23, GXV_SETTINGS_EVENTS) the system has handed over a new parameter (SCE_APP_EVENT_PARAM_NOTIFY): the next read takes a Settings
// parameter as a new press even when its text is the one already acted on (a second tap in one run, or a run started from the tile)
bool sFreshParameter = false;

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

// The title-screen picture for the menu backdrop, read in bounded chunks and averaged 2x2 into one 480x272 buffer.
// Any problem (no file, wrong size, short or failed read, no memory) gives an empty buffer: the menu keeps its plain
// background and shows no error. Allocation is non-throwing like the fonts', never the fatal operator new path.
FontBuffer<uint32_t> LoadBackdrop(const FontAllocator& allocator = {}) {
    FontBuffer<uint32_t> backdrop;
    FileInfo info{};
    if (Stat(nullptr, kBackdropPath, info) < 0 || !info.regular || info.size != kBackdropSourceBytes) return backdrop;
    FontBuffer<uint8_t> chunk;
    constexpr size_t chunkBytes = size_t(kBackdropChunkRows) * kBackdropSourceWidth * 4;
    if (!chunk.Allocate(chunkBytes, allocator) || !backdrop.Allocate(kBackdropPixels, allocator)) return {};
    const int64_t handle = OpenRead(nullptr, kBackdropPath);
    if (handle < 0) return {};
    bool valid = true;
    for (unsigned row = 0; valid && row < kBackdropSourceHeight; row += kBackdropChunkRows) {
        size_t offset = 0;
        while (offset < chunkBytes) {
            const int64_t count = Read(nullptr, handle, chunk.begin() + offset, chunkBytes - offset);
            if (count <= 0 || static_cast<uint64_t>(count) > chunkBytes - offset) { valid = false; break; }
            offset += static_cast<size_t>(count);
        }
        if (valid) DownsampleBackdropRows(chunk.begin(), row, kBackdropChunkRows, backdrop.begin());
    }
    const int closed = Close(nullptr, handle);
    if (!valid || closed < 0) return {};
    CleanBackdropPrompt(backdrop.begin());
    return backdrop;
}

// The HD texture pack (GXV_HDTEX_PATH, default ux0:data/smg/hdtex.pack, as vgx_hdtex.cpp reads it): one stat, so the HD textures row can say
// when there is nothing to turn on.
bool HdPackInstalled() {
    const char* path = std::getenv("GXV_HDTEX_PATH");
    if (!path || !*path) path = "ux0:data/smg/hdtex.pack";
    FileInfo info{};
    return Stat(nullptr, path, info) >= 0 && info.regular && info.size > 0;
}

// polish23 (8 Nov): the menu's memory. In play the console has 2-4 MiB of video memory (CDRAM) free (vitaGL's pool takes the rest; the
// log's "[vgx-vita] memory status" lines: cdram 2048-4096 KiB), and the two 960x544 display buffers asked for 4 MiB in one block: the
// in-game open failed whenever less than a whole free 4 MiB was there ("settings: the menu could not open"). Now, in this order:
//   - two buffers in one CDRAM block (as before; tear-free flips),
//   - ONE buffer (2 MiB, what the Strikers port's in-game Settings takes): the first picture is flipped to, each next one is copied in
//     right after a vertical blank (2 MiB in ~2 ms from the top, ahead of the scan: no tear in practice),
//   - no CDRAM at all: the game's own front buffer (vitaGL's, idle: the renderer is held and its display queue finished), the same
//     single-buffer copy; it is still the front buffer when the menu closes and the game's next swap replaces it
//     (GXV_SETTINGS_FRONTBUF=0: not this one; the open then waits for memory).
// The drawing buffer comes from the C heap, or else from a main-memory block outside it (free main memory is ~40 MiB in play).
// The memory is taken BEFORE the lease (Reserve), so a shortage costs the game no hold at all; the open is tried again later.
struct Screen {
    SceUID memory = -1, pixelsBlock = -1;
    void* base = nullptr;
    uint32_t* pixels = nullptr;
    SceDisplayFrameBuf previous{};
    unsigned next = 0, frames = 0;  // display buffers of our own: 2, 1, or 0 (the game's front buffer)
    bool published = false, pendingFrame = false, front = false, reserved = false;
    int inputSource = -2;
    unsigned controllerType = 0;
    const char* why = "";  // what the last failed Reserve / Acquire lacked (for the log line)
    Screen() = default;
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;
    ~Screen() { if (!published) FreeMemory(); }  // (a published buffer is the display's until Release restores the old one)
    static bool FrontBufferAllowed() {
        static const bool on = vgx::EnvInt("GXV_SETTINGS_FRONTBUF", 1) != 0;
        return on;
    }
    bool Reserve() {
        if (reserved) return true;
        pixels = static_cast<uint32_t*>(std::calloc(kPixels, sizeof(uint32_t)));
        if (!pixels) {
            void* p = nullptr;
            const size_t bytes = (kFrameBytes + 4095) & ~size_t(4095);
            pixelsBlock = sceKernelAllocMemBlock("galaxy_settings_draw", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, bytes, nullptr);
            if (pixelsBlock < 0 || sceKernelGetMemBlockBase(pixelsBlock, &p) < 0 || !p) {
                if (pixelsBlock >= 0) sceKernelFreeMemBlock(pixelsBlock);
                pixelsBlock = -1;
                why = "no memory for the menu's drawing buffer (2 MiB: C heap and main memory)";
                return false;
            }
            pixels = static_cast<uint32_t*>(p);
            std::memset(pixels, 0, kFrameBytes);
        }
        frames = 0;
        for (unsigned want = 2; want >= 1 && memory < 0; --want) {
            const size_t bytes = (kFrameBytes * want + 262143) & ~size_t(262143);
            memory = sceKernelAllocMemBlock("galaxy_settings", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, bytes, nullptr);
            if (memory >= 0 && (sceKernelGetMemBlockBase(memory, &base) < 0 || !base)) {
                sceKernelFreeMemBlock(memory);
                memory = -1;
                base = nullptr;
            }
            if (memory >= 0) frames = want;
        }
        if (!frames && !FrontBufferAllowed()) {
            FreeMemory();
            why = "no video memory for the menu's display buffer (2 MiB) and GXV_SETTINGS_FRONTBUF=0";
            return false;
        }
        reserved = true;
        return true;
    }
    bool Acquire(bool running) {
        if (!Reserve()) return false;
        previous = {};
        previous.size = sizeof(previous);
        const int got = sceDisplayGetFrameBuf(&previous, SCE_DISPLAY_SETBUF_NEXTFRAME);
        if (got < 0) { previous = {}; previous.size = sizeof(previous); }
        if (running && !previous.base) { why = "the display reported no current picture"; return false; }
        front = false;
        next = 0;
        if (!frames) {
            // (the game's front buffer: only a 960x544 32-bit one, which is what vitaGL shows)
            if (!running || !previous.base || previous.width != 960 || previous.height != 544 || previous.pitch < 960 ||
                previous.pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8) {
                why = "no video memory for the menu's display buffer, and the game's front buffer is not a 960x544 32-bit one";
                return false;
            }
            front = true;
        }
        return true;
    }
    // Waits for the start of the next vertical blank and copies the picture into a buffer that is (or is about to be) scanned out.
    bool CopyAfterVblank(const Surface& surface, uint8_t* buffer, unsigned pitchPixels) {
        if (sceDisplayWaitVblankStart() < 0) return false;
        for (unsigned y = 0; y < 544; ++y)
            std::memcpy(buffer + size_t(y) * pitchPixels * 4, surface.pixels + y * surface.stride, 960 * 4);
        return true;
    }
    bool Present(const Surface& surface) {
        if (surface.width != 960 || surface.height != 544 || surface.stride < 960 || !surface.pixels) return false;
        if (front) return CopyAfterVblank(surface, static_cast<uint8_t*>(previous.base), previous.pitch);
        // A failed wait leaves display ownership unknown. Do not overwrite a
        // possibly scanned-out buffer until a later vblank confirms retirement.
        if (pendingFrame && sceDisplayWaitVblankStart() < 0) return false;
        pendingFrame = false;
        if (frames == 1 && published) return CopyAfterVblank(surface, static_cast<uint8_t*>(base), 960);
        auto* buffer = static_cast<uint8_t*>(base) + next * kFrameBytes;
        for (unsigned y = 0; y < 544; ++y)
            std::memcpy(buffer + y * 960 * 4, surface.pixels + y * surface.stride, 960 * 4);
        SceDisplayFrameBuf frame{};
        frame.size = sizeof(frame); frame.base = buffer; frame.pitch = 960;
        frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8; frame.width = 960; frame.height = 544;
        if (sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0) return false;
        published = pendingFrame = true;
        if (frames == 2) next ^= 1;
        if (sceDisplayWaitVblankStart() < 0) return false;
        pendingFrame = false;
        return true;
    }
    void FreeMemory() {
        if (memory >= 0) sceKernelFreeMemBlock(memory);
        memory = -1; base = nullptr; frames = 0;
        if (pixelsBlock >= 0) sceKernelFreeMemBlock(pixelsBlock);
        else std::free(pixels);
        pixelsBlock = -1; pixels = nullptr;
        reserved = false;
    }
    bool Release() {
        if (published) {
            if (sceDisplaySetFrameBuf(previous.base ? &previous : nullptr, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0) return false;
            pendingFrame = true;
            if (sceDisplayWaitVblankStart() < 0) return false;
        }
        published = pendingFrame = front = false;
        FreeMemory();
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
    // The same choice the game made at start-up, so the menu shows the profile that is in use (and is not "changed" by it).
    port::settings::ApplyPadDefaults(sDocument.config, initial.caps.connected && initial.detectedProfile == port::controls::ProfileId::PlayStation);
    SettingsSession session{mode, sDocument, sActiveConfig, ReadLanguages(), sOverrides, initial.caps,
        {port_vgx_vita_fixed_cap_supported() != 0, port::controls::RemapEnabled()},
        {screen.pixels, 960, 544, 960}, {&screen, Poll, Present, HostNow, Wait}, kPath, &kFiles, &api};
    session.statusMessage = sStatus.empty() ? loaded.message : sStatus;
    session.detectedProfile = initial.detectedProfile;
    const auto fonts = LoadMenuFonts();
    session.fonts = fonts.view();
    // Freed when the menu closes (end of this function), in both LiveArea and in-game modes.
    const auto backdrop = LoadBackdrop();
    if (backdrop.size() == kBackdropPixels) { session.backdrop = backdrop.begin(); session.backdropW = kBackdropWidth; session.backdropH = kBackdropHeight; }
    // The menu's unchanging picture (backdrop, header band, heading) is built once into one scratch picture and copied each frame; with no
    // memory for it the menu simply draws it every frame. Freed with the backdrop when the menu closes.
    FontBuffer<uint32_t> layer;
    if (layer.Allocate(kPixels, FontAllocator{})) session.layer = layer.begin();
    session.hdtexPack = HdPackInstalled();
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
    static char logged[1024] = {};
    if (rc >= 0 && parameter[0] && std::strncmp(logged, parameter, sizeof(logged)) != 0) { // (console evidence of what the LiveArea passes)
        std::memcpy(logged, parameter, sizeof(logged)); logged[sizeof(logged) - 1] = 0;
        port_log_summary("settings: app parameter \"%.120s\" (argc=%d)", parameter, argc);
    }
    const LaunchRequest fromParameter = rc >= 0 ? ParseLaunch(0, nullptr, parameter, sizeof(parameter)) : LaunchRequest::Play;
    const bool fresh = sFreshParameter;  // (polish23: a PARAM_NOTIFY came: whatever the text, this is a new press)
    sFreshParameter = false;
    const bool repeat = !fresh && rc >= 0 && consumed[0] && std::strncmp(consumed, parameter, sizeof(consumed)) == 0;
    if (rc < 0 || fromParameter != LaunchRequest::Settings) consumed[0] = 0;
    else std::memcpy(consumed, parameter, sizeof(consumed));
    if (repeat && !argc) return LaunchRequest::Play;
    return ParseLaunch(argc, argv, rc >= 0 ? parameter : nullptr, rc >= 0 ? sizeof(parameter) : 0);
}
// (polish23) whether the parameter is a Settings one now, repeat or not, without touching TakeLaunch's state.
bool ParameterSaysSettings() {
    char parameter[1024] = {};
    const int rc = sceAppMgrGetAppParam(parameter);
    return rc >= 0 && parameter[0] && ParseLaunch(0, nullptr, parameter, sizeof(parameter)) == LaunchRequest::Settings;
}
// What the system tells a RUNNING application when the LiveArea Settings tile is tapped (psla:-settings). Strikers 1.3 (np_vita_settings.cpp Pump,
// 73c1659) reads only sceAppMgrGetAppParam every 250 ms on a frame hook and it works on the console, so the parameter is the carrier; the system
// events are used here to (a) read it at once when the application is resumed or a parameter arrives instead of up to a poll later, (b) log
// exactly what the system sent (GXV_SETTINGS_EVENTS=2) so a console log settles what the tap does, and (c) never leave a quit request unanswered:
// REQUEST_QUIT is read and, unless a Settings parameter came with it, honoured by leaving at once. GXV_SETTINGS_EVENTS=0 restores the old poll.
struct AppEvents { bool wake = false; bool quit = false; bool param = false; };
AppEvents ReadAppEvents(int logMode) {
    AppEvents out;
    static unsigned logged = 0;
    for (int i = 0; i < 16; ++i) {
        SceAppMgrSystemEvent system{};
        if (sceAppMgrReceiveSystemEvent(&system) < 0) break;
        const unsigned type = static_cast<unsigned>(system.systemEvent);
        if (logMode && logged < 40) { ++logged; port_log_summary("settings: system event 0x%08X", type); }
        if (type == SCE_APPMGR_SYSTEMEVENT_ON_RESUME || type == SCE_APPMGR_SYSTEMEVENT_ON_ACTIVATE) out.wake = true;
        else if (type == SCE_APPMGR_SYSTEMEVENT_REQUEST_QUIT) out.quit = true;
    }
    for (int i = 0; i < 16; ++i) {
        SceAppMgrEvent app{};
        if (sceAppMgrReceiveEvent(&app) < 0) break;
        const unsigned type = static_cast<unsigned>(app.event);
        if (logMode && logged < 40) { ++logged; port_log_summary("settings: app event 0x%08X", type); }
        if (type == SCE_APP_EVENT_PARAM_NOTIFY) out.param = true;
        if (type == SCE_APP_EVENT_PARAM_NOTIFY || type == SCE_APP_EVENT_ON_RESUME || type == SCE_APP_EVENT_ON_ACTIVATE) out.wake = true;
        else if (type == SCE_APP_EVENT_REQUEST_QUIT) out.quit = true;
    }
    return out;
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
    const char* failed = nullptr;  // (polish23) what the last failed acquire was waiting for, for the log line
    static bool Acquire(void* p, Resource resource, unsigned timeoutMs) {
        auto& self = *static_cast<RuntimeLease*>(p);
        const bool ok = AcquireOne(self, resource, timeoutMs);
        if (!ok) {
            switch (resource) {
            case Resource::Input: self.failed = "the input thread did not hand over the pad in time"; break;
            case Resource::Audio: self.failed = "the audio output did not fall silent in time"; break;
            case Resource::Renderer: self.failed = "the renderer did not finish its pictures in time (a long shader build or picture-size change)"; break;
            case Resource::Scanout: self.failed = self.screen.why; break;
            default: self.failed = "a game resource could not be held"; break;
            }
        }
        return ok;
    }
    static bool AcquireOne(RuntimeLease& self, Resource resource, unsigned timeoutMs) {
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
        case Resource::Audio:
            self.audio = port_vita_audio_is_held();
            if (port_vita_audio_hold(1, timeoutMs) != 0) return true;
            // (polish23: an output that has failed (an error from the device, e.g. after a sleep) plays nothing and cannot acknowledge a
            // hold; the game's audio recovery reopens it later. Nothing audible to hold: the menu may open. Before, every attempt failed.)
            if (port_vita_audio_failed()) { port_vita_audio_hold(0, timeoutMs); return true; }
            return false;
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
    // The stale-settings guard (port/settings_guard.h): a file from an older release is stamped, backed up once and its changed lines written out, before it is loaded. One log line.
    {
        const auto guard = port::settings::RunSettingsGuard(kPath, GALAXY_SETTINGS_VERSION, api, kFiles);
        if (!guard.line.empty()) port_log_summary("%s", guard.line.c_str());
        // setfix31: from now on every writer of the file (the in-game Settings' save, its restart path and Put Everything Back, the old menu) stamps it
        // with this release, so the next start does not take it for a 2.2.1 file. Not when the guard could not migrate it: that file stays as it is.
        if (port::settings::GuardFileIsCurrent(guard.outcome)) port::settings::SetWriterStamp(port::settings::GuardStampLine(GALAXY_SETTINGS_VERSION));
    }
    LoadDocument(kPath, api, kFiles, sDocument);
    sActiveConfig = sDocument.config;
    // A pad on a PS TV, with the profile never chosen by hand, plays with the PlayStation profile (R2 shoots, right stick aims).
    port::settings::ApplyPadDefaults(sActiveConfig, port_vita_input_external_pad() != 0);
    {   // The player's buttons, before anything reads the active mapping. setfix31: NativeStartup below (NativeHostGet -> ActiveCapabilities) used to
        // run first and make the defaults the active mapping, so this was refused ("Active controls are immutable until restart") and a saved
        // button change, or a PS TV pad's profile, never applied at a start. Nothing below changes the profile or the buttons.
        std::string error;
        const unsigned profile = sActiveConfig.profile == port::controls::ProfileId::Handheld ? 0 : 1;
        if (!port::controls::SetActiveMapping(sActiveConfig.profiles[profile], sActiveConfig.profile, port::controls::NominalCaps(sActiveConfig.profile), error))
            port_log_summary("settings: active mapping rejected: %s", error.c_str());
    }
    port::settings::NativeStartup(sActiveConfig);  // settingsnative: recovery marker, Safe Saves on (the hidden rows keep the file's values), env snapshot (before ApplyStartup: env.txt still wins)
    sOverrides = ApplyStartup(sActiveConfig, true);
    {
        // The Picture mode and what it set (the basic log: a console run shows which mode it is and what env.txt changed of it).
        auto env = [](const char* k) { const char* v = std::getenv(k); return v ? v : "-"; };
        // (modes26: two picture modes, Performance = 60 and Quality = 30; the switches each one set)
        port_log_summary("settings: picture mode %s (GXV_PICTURE_MODE=%s): GXV_FPS_LIMIT=%s GXV_AUTOQ_TARGET=%s GXV_AUTOQ_MIN=%s GXV_AUTOQ_MAX=%s GXV_AUTOQ_MSAA=%s GXV_AUTOQ_EDGESTEP=%s "
                         "GXV_EDGE_AA_NATIVE=%s GXV_PACE23_LOCKED_EDGE=%s GXV_AUTOQ_FXSTEP=%s GXV_AUTOQ_LOCKED30_FLOOR=%s GXV_AUTOQ_LOCKED30_START=%s GXV_STEADY30=%s GXV_PERF_BUDGET=%s GXV_SMOOTH60=%s; %u env.txt override(s)",
                         port::settings::Mode30(sActiveConfig) ? "Quality (30)" : "Performance (60)", env("GXV_PICTURE_MODE"), env("GXV_FPS_LIMIT"), env("GXV_AUTOQ_TARGET"), env("GXV_AUTOQ_MIN"),
                         env("GXV_AUTOQ_MAX"), env("GXV_AUTOQ_MSAA"), env("GXV_AUTOQ_EDGESTEP"), env("GXV_EDGE_AA_NATIVE"), env("GXV_PACE23_LOCKED_EDGE"), env("GXV_AUTOQ_FXSTEP"),
                         env("GXV_AUTOQ_LOCKED30_FLOOR"), env("GXV_AUTOQ_LOCKED30_START"), env("GXV_STEADY30"), env("GXV_PERF_BUDGET"), env("GXV_SMOOTH60"), static_cast<unsigned>(sOverrides.size()));
        // (gfx26: GXV_QUALITY_SET, port/quality_set.h: what the picture-quality umbrella wrote; no line unless it was asked for)
        if (!port::settings::QualitySetCensus().empty()) port_log_summary("%s", port::settings::QualitySetCensus().c_str());
        if (!port::settings::Quality30Census().empty()) port_log_summary("%s", port::settings::Quality30Census().c_str());   // (q30-26)
        if (!port::settings::ModeKeysCensus().empty()) port_log_summary("%s", port::settings::ModeKeysCensus().c_str());   // (deadfix27)
        if (!port::settings::PerfDefaultsCensus().empty()) port_log_summary("%s", port::settings::PerfDefaultsCensus().c_str());   // (pace33 PATCH8)
    }
    if (request == LaunchRequest::Settings && port_settings_native_request && port_settings_native_request(1)) return 0;  // settingsnative: a stale tile opens the native Settings at the entry screen
    if (request != LaunchRequest::Settings) return 0;
    port_dvd_set_root("ux0:data/smg/game");
    if (!port_vita_input_raw_init()) return 1;
    for (;;) {
        Screen screen;
        // (polish23: never a closed game for want of memory: the game starts instead; at a cold start the video memory is all free)
        if (!screen.Acquire(false)) {
            port_log_summary("settings: screen allocation failed (%s); starting the game instead", screen.why);
            screen.Release();
            return 0;
        }
        const auto result = MenuAndRetire(screen, OpenMode::ColdStart);
        if (result.intent != ExitIntent::Play) return 1;
        Relaunch();
    }
}
extern "C" void port_settings_request(void) {
    if (!Enabled()) return;
    // (polish23: a request that was not pending already starts a new series of attempts on the game thread)
    if (!sRequested.exchange(true, std::memory_order_acq_rel)) sRequestSeq.fetch_add(1, std::memory_order_acq_rel);
}
extern "C" int port_settings_active(void) { return sActive.load(std::memory_order_acquire) ? 1 : 0; }
extern "C" int port_settings_input_service(void) {
    if (!Enabled()) return 0;
    const unsigned generation = sInputHold.Requested();
    if (generation && sInputHold.Acknowledge(generation)) {
        while (sInputHold.Requested() == generation) sceKernelDelayThread(1000);
        return 1;
    }
    // Strikers polls the parameter every 250 ms (np_vita_settings.cpp Pump); once a second left a tap that the system answers by resuming the
    // application for a short moment unread. GXV_SETTINGS_POLL_MS (default 250), GXV_SETTINGS_EVENTS (default 1; 0 = no event reads, 2 = log them).
    static const uint64_t pollUs = uint64_t(std::max<long>(50, std::min<long>(2000, vgx::EnvInt("GXV_SETTINGS_POLL_MS", 250)))) * 1000;
    static const int eventMode = (int)vgx::EnvInt("GXV_SETTINGS_EVENTS", 2);  // (bugs26: 2 = read and log the first 40 events; the console has never shown what the tile sends a running game, so every player log now does)
    static uint64_t pollAfter = 0, eventAfter = 0;
    // (polish23) A quit request is never answered by leaving at once: the LiveArea's Settings tile must never close the game (issue 2). It
    // is honoured only after a grace of GXV_SETTINGS_QUIT_MS (default 750) in which no Settings parameter has shown up, no Settings request
    // is pending or open and the system has handed over no new parameter; GXV_SETTINGS_QUIT=0: never left this way (logged only).
    static const long quitMode = vgx::EnvInt("GXV_SETTINGS_QUIT", 0);
    static const uint64_t quitGraceUs = uint64_t(std::max<long>(100, std::min<long>(5000, vgx::EnvInt("GXV_SETTINGS_QUIT_MS", 750)))) * 1000;
    static uint64_t quitAt = 0, quitCheckAfter = 0;
    bool now_ = false;
    if (eventMode && Now() >= eventAfter) {
        eventAfter = Now() + 50000;
        const AppEvents events = ReadAppEvents(eventMode);
        now_ = events.wake || events.quit;
        if (events.param) sFreshParameter = true;
        if (events.quit && !quitAt) {
            quitAt = Now() + quitGraceUs;
            port_log_summary("settings: quit requested by the system; %s", quitMode ? "leaving unless a Settings request comes with it" : "not leaving (GXV_SETTINGS_QUIT=0)");
        }
    }
    if (quitAt && Now() >= quitCheckAfter) {
        quitCheckAfter = Now() + 50000;  // (the parameter is read every 50 ms during the grace)
        bool settings = sRequested.load(std::memory_order_acquire) || sActive.load(std::memory_order_acquire);
        if (!settings && TakeLaunch() == LaunchRequest::Settings) {  // (a new Settings press: the tile)
            port_log_summary("settings: the LiveArea's Settings button (with a quit request)");
            port_settings_request();
            settings = true;
        }
        // (a Settings parameter already acted on, e.g. the one this run was started with: not leaving either; a genuine quit is then the
        // system's to carry out, as for a game that does not read the request)
        const bool stay = settings || ParameterSaysSettings();
        if (stay || !quitMode) {
            port_log_summary("settings: quit request not honoured (%s)", settings ? "Settings is asked for" : stay ? "a Settings parameter is present" : "GXV_SETTINGS_QUIT=0");
            quitAt = 0;
            pollAfter = Now() + pollUs;
        } else if (Now() >= quitAt) {
            port_log_summary("settings: quit requested by the system; leaving");
            // MP3 (merge fix 5): a multiplayer session is told goodbye (the others would time this console out after 15 s) and a multiplayer save
            // write gets a moment to finish; bounded to 300 ms in all, and nothing at all when multiplayer was never entered.
            if (mp_port_quit) mp_port_quit(300);
            port_log_flush();
            sceKernelExitProcess(0);
        }
    }
    if (now_ || Now() >= pollAfter) {
        pollAfter = Now() + pollUs;
        if (TakeLaunch() == LaunchRequest::Settings) { port_log_summary("settings: the LiveArea's Settings button"); port_settings_request(); }
    }
    return 0;
}
// MP3: no Settings menu during a multiplayer session (it holds the game thread for seconds: the others would time this console
// out, and a restart would leave the session). The request is dropped with a toast; single-player is unchanged. True = refused.
// (One small helper, so the host tests that copy the body of port_settings_game_boundary only have to stub this.)
static bool MpRefuseSettings() {
    if (!mp_active || !mp_active()) return false;
    sRequested.store(false, std::memory_order_release);
    port_log_summary("settings: refused during a multiplayer session");
    if (MpUi::toast) MpUi::toast(0x0306, -1, 0);
    return true;
}
extern "C" void port_settings_game_boundary(int safe) {
    if (sRequested.load(std::memory_order_acquire) && MpRefuseSettings()) return;
    if (port_settings_native_request && sRequested.exchange(false)) { port_settings_native_request(0); return; }  // settingsnative: the stale tile (or a second press) goes to the native Settings, never to the old menu
    if (!Enabled() || !safe || !sRequested.load(std::memory_order_acquire) || !port_boot_title_is_visible() || port_vgx_vita_splash_active() || Now() < sRetryAfter) return;
    // GXV_SETTINGS_SAVE_GUARD=1 defers entry during saves; default off for A/B.
    static const bool saveGuard = vgx::EnvInt("GXV_SETTINGS_SAVE_GUARD", 0) != 0;
    // Keep the request pending while guest turns finish an active save. Waiting
    // after the lease freezes interrupts would prevent that sequence finishing.
    if (saveGuard && !GameSystemFunction::isPrepareResetSaveDataHandleSequence()) return;
    PortHostAllocScope allocations;
    RuntimeLease context;
    Hooks hooks{&context, RuntimeLease::Acquire, RuntimeLease::Release};
    Lease lease;
    // (polish23) A request is tried until it opens: after a failed attempt again 1, 2, 4, then every 8 s, for GXV_SETTINGS_RETRY_S (default
    // 180) from the request; the renderer and the pad get 250 ms on the first attempt, 500 ms on the second, 1 s from the third. Each
    // failure is one log line naming what was missing. Before: three attempts one second apart, then "the menu could not open (3
    // attempts); request dropped" (the usual cause: no 4 MiB of video memory for the menu's two display buffers, see Screen).
    static unsigned failedOpens = 0, seenSeq = 0;
    static uint64_t requestedAt = 0;
    const unsigned seq = sRequestSeq.load(std::memory_order_acquire);
    if (seq != seenSeq || !requestedAt) {
        seenSeq = seq;
        failedOpens = 0;
        requestedAt = Now();
    }
    static const uint64_t retryForUs = uint64_t(std::max<long>(10, std::min<long>(3600, vgx::EnvInt("GXV_SETTINGS_RETRY_S", 180)))) * 1000000;
    const unsigned timeoutMs = failedOpens == 0 ? 250u : failedOpens == 1 ? 500u : 1000u;
    // The memory first, while the game still runs: a shortage then holds nothing.
    const bool memory = context.screen.Reserve();
    sActive.store(memory, std::memory_order_release);
    if (!memory || !lease.Acquire(hooks, timeoutMs)) {
        const char* why = !memory ? context.screen.why : (context.failed ? context.failed : "unknown");
        context.screen.Release();  // (the reserved memory; nothing was published)
        sActive.store(false, std::memory_order_release);
        ++failedOpens;
        const uint64_t waitUs = uint64_t(1000000) << std::min(failedOpens - 1, 3u);
        if (Now() - requestedAt >= retryForUs) {
            sRequested.store(false, std::memory_order_release);
            port_log_summary("settings: the menu could not open in %u attempts over %u s (last: %s); request dropped, the game goes on (press Settings again)",
                             failedOpens, unsigned((Now() - requestedAt) / 1000000), why);
            failedOpens = 0;
            requestedAt = 0;
            return;
        }
        sRetryAfter = Now() + waitUs;
        port_log_summary("settings: the menu could not open yet (attempt %u: %s); trying again in %u s", failedOpens, why, unsigned(waitUs / 1000000));
        return;
    }
    failedOpens = 0;
    requestedAt = 0;
    sRequested.store(false, std::memory_order_release);
    // (GXV_PAR_PLANT_EARLY: a plant batch listed at the end of the last turn ends before the menu, which may change the game)
    if (port_par_plant_close_early) port_par_plant_close_early();
    const uint64_t opened = Now();
    port_log_summary("settings: gameplay held; menu time is not gameplay performance");
    context.uiOwned = true;
    const auto result = MenuAndRetire(context.screen, OpenMode::Running);
    lease.Release(hooks);
    sActive.store(false, std::memory_order_release);
    port_log_summary("settings: gameplay resumed after %.2f s; exclude the crossing FPS window", (Now()-opened)/1000000.0);
    if (result.intent == ExitIntent::Restart) {
        Relaunch();
        port_settings_request();  // (the restart failed: the menu again, with its message; a new series of attempts)
    }
}

// ---- settingsnative: the host of the native in-game Settings menu (platform/include/port/settings_native.h); append-only block ----
extern "C" void port_card_write_flush(unsigned waitMs) __attribute__((weak));
extern "C" unsigned port_card_write_pending(void) __attribute__((weak));
extern "C" void port_zoom_save_now(void) __attribute__((weak));   // (perf.cpp, setnat-pause)
extern "C" int native_apputil_init(void* init, void* boot) __asm__("sceAppUtilInit") __attribute__((weak));
extern "C" int native_sys_param_int(int id, int* value) __asm__("sceAppUtilSystemParamGetInt") __attribute__((weak));
namespace {
bool NativeRelaunch() {   // (the menu has polled relaunchReady frame by frame; nothing waits here)
    if (mp_port_quit) mp_port_quit(300);
    if (port_card_write_flush) port_card_write_flush(3000);   // the queued card writes (GXV_CARD_ASYNC) land before the process goes
    if (port_zoom_save_now) port_zoom_save_now();             // a zoom change otherwise waits 45 idle turns
    port_log_flush();
    return Relaunch();
}
extern "C" unsigned port_card_write_finished(void) __attribute__((weak));
extern "C" unsigned port_card_write_failures(void) __attribute__((weak));
extern "C" unsigned port_nand_progress(void) __attribute__((weak));
extern "C" unsigned port_nand_failures(void) __attribute__((weak));
extern "C" bool port_nand_all_atomic(void) __attribute__((weak));
// setfix31b: the restart wait's view of the save layer (see NativeHost::saveProgress)
unsigned NativeSaveProgress() { return (port_card_write_finished ? port_card_write_finished() : 0) + (port_nand_progress ? port_nand_progress() : 0); }
unsigned NativeSaveFailures() { return (port_card_write_failures ? port_card_write_failures() : 0) + (port_nand_failures ? port_nand_failures() : 0); }
// Starting again during a save is safe only with Safe Saves (the game save goes to GameData.bin.mptmp and is swapped in: a cut, which a LoadExec is,
// leaves the old save or the new one, and the boot repair puts a .mptmp back when the cut fell between the renames; savelife cut test 0 of 300 torn).
bool NativeRelaunchSafe() { return port_nand_all_atomic && port_nand_all_atomic(); }
bool NativeRelaunchReady() { return GameSystemFunction::isPrepareResetSaveDataHandleSequence() && !(port_card_write_pending && port_card_write_pending()); }
const char* NativeSystemLanguage() {   // SCE_SYSTEM_PARAM_ID_LANG = 1; the ids map to the disc's language names
    static bool initialised = false;
    if (!initialised && native_apputil_init) {   // (the same set-up the dialogs do; BUSY means it already ran)
        alignas(8) unsigned char init[0x40] = {}, boot[0x28] = {};
        native_apputil_init(init, boot);
        initialised = true;
    }
    int id = -1;
    if (!native_sys_param_int || native_sys_param_int(1, &id) < 0) return nullptr;
    return port_system_language_name(id);
}
}
namespace port::settings {
const NativeHost* NativeHostPlatform() {
    static NativeHost host;
    host = {kPath, &kFiles, &sActiveConfig, &sOverrides, NativeRelaunch, Enabled(), port_vgx_vita_fixed_cap_supported() != 0, port::controls::ActiveCapabilities().motion,
            NativeLiveOpsPort(), NativeRelaunchReady, NativeSystemLanguage, NativeEnvWasPresent, port_vita_input_external_pad() != 0,
            NativeSaveProgress, NativeSaveFailures, NativeRelaunchSafe};
    return &host;
}
}
