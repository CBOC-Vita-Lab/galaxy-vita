// Galaxy 3.0 multiplayer (port side, owner: integ): the session layer at port boot and exit.
//   mp_port_boot(dataRoot): mp_core_boot() (mp.cfg) and mp_core_set_data_hash(): CRC-32 over the sorted list of the files under
//   StageData/ in the disc's file table (sys/fst.bin: "path:size\n" per file, lower-case paths). Two consoles with the same
//   stages get the same value whatever their region or language folders; a modded / different StageData refuses to play together
//   (the session fingerprint, DESIGN.md section 3). Reads one file once at boot; nothing else runs until a session starts.
//   mp_port_shutdown(): leaves a running session (BYE) and stops the helper thread.
#include "mp_core.h"
#include "mp_core_sys.h"
#include "port/mp_keepsafe.h"   // MP3 keepsafe
#include "mp_savefile.h"
#include "mp_posix_io.h"   // smallfix2
#include "port/port.h"   // port_host_time_ns (memfix boot timing)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

extern "C" size_t port_dvd_cached_fst_copy(const char* root, uint8_t* out, size_t cap) __attribute__((weak));

namespace {

uint32_t Be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// 0 when the table cannot be read (the fingerprint then only covers build + protocol).
uint32_t StageDataHash(const char* dataRoot, int* filesOut) {
    *filesOut = 0;
    if (!dataRoot) return 0;
    std::vector<uint8_t> fst;
    if (port_dvd_cached_fst_copy) {
        const size_t n = port_dvd_cached_fst_copy(dataRoot, nullptr, 0);
        if (n >= 12 && n < (32u << 20)) {
            fst.resize(n);
            if (port_dvd_cached_fst_copy(dataRoot, fst.data(), fst.size()) != n) fst.clear();
        }
    }
    if (fst.empty()) {   // smallfix2: POSIX calls (the game's fopen is wrapped on the Vita)
        const std::string path = std::string(dataRoot) + "/sys/fst.bin";
        long len = -1;
        char probe;
        if (mp_io_read(path.c_str(), &probe, 0, 0, &len) >= 0 && len >= 12 && len < (32 << 20)) {
            fst.resize((size_t)len);
            if (mp_io_read(path.c_str(), fst.data(), fst.size()) != (long)fst.size()) fst.clear();
        }
    }
    if (fst.size() < 12) return 0;
    const uint32_t count = Be32(&fst[8]);
    if (count == 0 || (uint64_t)count * 12 >= fst.size()) return 0;
    const char* names = (const char*)&fst[(size_t)count * 12];
    const size_t namesSize = fst.size() - (size_t)count * 12;
    auto nameAt = [&](uint32_t off, std::string* out) -> bool {
        if (off >= namesSize) return false;
        const char* s = names + off;
        const size_t max = namesSize - off;
        const size_t n = strnlen(s, max);
        if (n == max) return false;
        out->assign(s, n);
        for (char& c : *out) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        return true;
    };
    // walk: a stack of (end index, path prefix) for the open directories
    struct Dir { uint32_t end; std::string prefix; };
    std::vector<Dir> stack;
    stack.push_back(Dir{count, std::string()});
    std::vector<std::string> lines;
    for (uint32_t i = 1; i < count; i++) {
        while (stack.size() > 1 && i >= stack.back().end) stack.pop_back();
        const uint8_t* e = &fst[(size_t)i * 12];
        std::string name;
        if (!nameAt(Be32(e) & 0x00FFFFFFu, &name)) return 0;
        const std::string full = stack.back().prefix + name;
        if (e[0] != 0) {   // directory: entries up to (not including) `next`
            const uint32_t next = Be32(e + 8);
            if (next <= i || next > count || stack.size() > 32) return 0;
            if (stack.size() == 1 && name != "stagedata") { i = next - 1; continue; }
            stack.push_back(Dir{next, full + "/"});
            continue;
        }
        if (full.compare(0, 10, "stagedata/") != 0) continue;
        char size[16];
        snprintf(size, sizeof size, ":%u\n", (unsigned)Be32(e + 8));
        lines.push_back(full + size);
    }
    std::sort(lines.begin(), lines.end());
    uint32_t crc = 0;
    for (const std::string& l : lines) crc = mp_crc32(crc, l.data(), l.size());
    *filesOut = (int)lines.size();
    return crc;
}

}  // namespace

extern "C" const char* mp_service_app_version(void) __attribute__((weak));
// MP3 keepfix (keepsafe review 4): the game's in-memory Team File (MpSaveGame.cpp) and the menus' flow.cfg / coop.cfg caches (mp_ui_flow.cpp)
// are reloaded after keepsafe replaced files; MpSaveGame says when the data may be replaced at all. Weak: tools without the game link.
extern "C" bool mp_savegame_keepsafe_may_replace(void) __attribute__((weak));
extern "C" void mp_savegame_keepsafe_reload(void) __attribute__((weak));
extern "C" void mp_ui_flow_forget(void) __attribute__((weak));

// The hooks keepsafe calls (a function of its own so the host tests check the wiring: tests/mp/keepsafe_test.cpp, "Wiring").
extern "C" void mp_port_keepsafe_wire(void) {
    if (mp_savegame_keepsafe_reload) mp_keepsafe_add_reload_hook(mp_savegame_keepsafe_reload);
    if (mp_ui_flow_forget) mp_keepsafe_add_reload_hook(mp_ui_flow_forget);
    if (mp_savegame_keepsafe_may_replace) mp_keepsafe_set_guard(mp_savegame_keepsafe_may_replace);
}

extern "C" void mp_aids_boot(void) __attribute__((weak));   // MP3 hwaids: platform/src/mp/mp_hwaids.cpp (the UDP log mirror switch)
extern "C" void mp_port_boot(const char* dataRoot) {
    if (mp_aids_boot) mp_aids_boot();   // MP3 hwaids: nothing unless SMG_LOG_UDP is set
    // MP3 keepsafe: BEFORE anything of ours writes (mp_core_boot makes mp.cfg's console key): a first start of a new version makes ONE verified
    // copy of the multiplayer data (it never changes a live file). A normal start costs one small file read and at most two stats; single player pays the same.
    mp_port_keepsafe_wire();
    mp_keepsafe_boot(mp_service_app_version ? mp_service_app_version() : "3.0.0", MP_BUILD_ID);
    mp_core_boot();
    int files = 0;
    const int64_t t0 = port_host_time_ns();   // memfix: what multiplayer costs at boot (the file table read, 81 KB, freed again)
    const uint32_t hash = StageDataHash(dataRoot, &files);
    mp_log("boot data hash=%08x stage files=%d ms=%d", (unsigned)hash, files, (int)((port_host_time_ns() - t0) / 1000000));
    mp_core_set_data_hash(hash);
    mp_keepsafe_set_data_hash(hash);
}

extern "C" void mp_port_shutdown(void) { mp_core_shutdown(); }

// Merge fix 5: the system asked the app to quit (vita/vgx-game/settings_runtime.cpp, before sceKernelExitProcess; main thread). Bounded: the whole
// call takes budgetMs at most (clamped to 300). 1. A session gets its goodbye (BYE flushed; peers would otherwise time this console out after 15 s).
// 2. A multiplayer save write in flight is given the rest of the budget to finish (the recovery checkpoint writer: mp_save_async_idle; the
// co-op write-back through its .mptmp file: mp_save_write_in_flight, WEAK (the save owner adds it; until then the wait is skipped
// and an unfinished write is recovered at the next boot as before). Returns true when the goodbye went out (or there was no session) and no save
// write was left running. mp_shutdown's unbounded steps (helper thread join, radio teardown, UPnP unmapping of 600 ms) are not run.
extern "C" bool mp_save_write_in_flight(void) __attribute__((weak));
bool mp_save_async_idle(void) __attribute__((weak));
extern "C" bool mp_port_quit(unsigned budgetMs) {
    // keepfix (keepsafe review 14): multiplayer never entered and no keepsafe operation running: nothing to say, nothing being written. A
    // keepsafe call (bringing an item back from a menu before any session) is waited for even then, within the same budget; one cut short
    // leaves each file it changes old or new (every change is one atomic file write).
    if (!mp_ever_entered() && !mp_keepsafe_busy()) return true;   // (optfix: ever entered: the flag may have been retired after a session)
    if (budgetMs > 300) budgetMs = 300;
    const uint64_t t0 = mp_sys_now_us();
    const bool said = mp_ever_entered() ? mp_core_goodbye(budgetMs) : true;
    bool busy = false;
    for (;;) {
        busy = (mp_save_async_idle && !mp_save_async_idle()) || (mp_save_write_in_flight && mp_save_write_in_flight()) ||
               mp_keepsafe_busy();   // (keepsafe: a call in progress; cut short, every file is old or new)
        if (!busy || mp_sys_now_us() - t0 >= (uint64_t)budgetMs * 1000u) break;
        mp_sys_sleep_ms(2);
    }
    mp_log("quit: goodbye %s, save write %s after %u ms", said ? "sent" : "not sent (core busy)", busy ? "STILL RUNNING" : "idle",
           (unsigned)((mp_sys_now_us() - t0) / 1000u));
    return said && !busy;
}

// "Multiplayer was entered in this boot" (port/mp_bridge.h mp_entered(), an inline bool) as a symbol the renderer library can reference
// weakly (it is also built without multiplayer): vgx_backend_gl.cpp stops recording the player warm-up list once this is true.
extern "C" int mp_port_entered(void) { return mp_ever_entered() ? 1 : 0; }   // (optfix: sticky)
