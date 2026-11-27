// Native Settings host for the desktop and armhf builds (the Vita's is the "settingsnative" block in vita/vgx-game/settings_runtime.cpp).
#ifndef __vita__
#include "port/settings_native.h"
#include "port/control_bindings.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" void port_log(const char* fmt, ...) __attribute__((weak));
extern "C" void port_host_alloc_scope_enter(void) __attribute__((weak));   // os_thread.cpp: `new` takes host memory, not the current game heap
extern "C" void port_host_alloc_scope_leave(void) __attribute__((weak));
extern "C" void port_handheld_pictures_changed(void) __attribute__((weak));   // platform/src/gx/vgx_galaxy.cpp: the button pictures are made again

namespace port::settings {
namespace {
int Err(int value) { return value < 0 ? -errno : value; }
int64_t OpenRead(void*, const char* p) { return Err(::open(p, O_RDONLY)); }
int64_t OpenWrite(void*, const char* p) { return Err(::open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666)); }
int64_t Read(void*, int64_t h, void* b, size_t n) { return Err(static_cast<int>(::read(static_cast<int>(h), b, n))); }
int64_t Write(void*, int64_t h, const void* b, size_t n) { return Err(static_cast<int>(::write(static_cast<int>(h), b, n))); }
int Sync(void*, int64_t h) { return Err(::fsync(static_cast<int>(h))); }
int Close(void*, int64_t h) { return Err(::close(static_cast<int>(h))); }
int Stat(void*, const char* p, FileInfo& info) {
    struct stat st{};
    if (::stat(p, &st) < 0) return -errno;
    info = {static_cast<uint64_t>(st.st_size), S_ISREG(st.st_mode) != 0};
    return 0;
}
int Rename(void*, const char* a, const char* b) { return Err(::rename(a, b)); }
int Remove(void*, const char* p) { return Err(::unlink(p)); }
const FileOps kFiles{nullptr, OpenRead, OpenWrite, Read, Write, Sync, Close, Stat, Rename, Remove};
bool Relaunch() {
    if (port_log) port_log("settings: restart requested");
    else std::printf("settings: restart requested\n");
    return false;
}
const char* SystemLanguage() {   // only PETARI_SYSTEM_LANGUAGE, never LANG
    const char* e = std::getenv("PETARI_SYSTEM_LANGUAGE");
    return e && *e ? e : nullptr;
}
bool EnvPresent(const char* key) { return key && std::getenv(key) != nullptr; }   // (the desktop applies nothing: the real environment is the snapshot)

// The desktop has no input thread to take a staged mapping at the top of its poll (the Vita's does, vita/runtime/input.cpp), so the swap is
// committed here, on the calling thread, and the pictures drawn on signs and prompts are made again, as on the Vita.
bool DesktopApplyBindings(void*, const Config& c, controls::ProfileId effective) {
    const NativeLiveOps* base = NativeLiveOpsPort();
    if (!base || !base->applyBindings || !base->applyBindings(base->context, c, effective)) return false;
    if (controls::CommitStagedMapping() && port_handheld_pictures_changed) port_handheld_pictures_changed();
    return true;
}
const NativeLiveOps* DesktopLiveOps() {
    static NativeLiveOps ops;
    static bool made = false;
    if (!made) {
        const NativeLiveOps* b = NativeLiveOpsPort();
        ops = *b;
        ops.applyBindings = DesktopApplyBindings;
        made = true;
    }
    return &ops;
}
}  // namespace

// Same directory rule as mp_config_dir (platform/src/mp/mp_config.cpp): $PETARI_DATA_DIR, else the current directory.
const NativeHost* NativeHostPlatform() {
    static char path[1100];
    static std::vector<Override> overrides;
    static Config active;
    static NativeHost host;
    static bool ready = false;
    if (!ready) {
        // The table and the vectors inside its Config live for the whole run: made on first use, inside a scene, they landed in that scene's heap and went with
        // it (Egg Star's pause read freed memory). Host memory, whatever heap is current.
        struct HostScope {
            HostScope() { if (port_host_alloc_scope_enter) port_host_alloc_scope_enter(); }
            ~HostScope() { if (port_host_alloc_scope_leave) port_host_alloc_scope_leave(); }
        } hostScope;
        const char* d = std::getenv("PETARI_DATA_DIR");
        std::snprintf(path, sizeof path, "%s/player-settings.cfg", d && *d ? d : ".");
        Document doc;
        LoadDocument(path, controls::GetControlApi(), kFiles, doc);   // the desktop applies nothing: the file as found now is what this run uses
        active = doc.config;
        host = {path, &kFiles, &active, &overrides, Relaunch, true, true, true, DesktopLiveOps(), nullptr, SystemLanguage, EnvPresent, false};
        ready = true;
    }
    return &host;
}
}  // namespace port::settings
#endif
