// Galaxy 3.0 multiplayer (port side, flow / wave 2b): what the simplified menus need beyond port/mp_bridge.h (port/mp_ui_flow.h).
// - mp_ui_net_info: Wi-Fi connected (the Join list scans Wi-Fi rooms, else consoles nearby), service configured, account made.
// - mp_ui_rejoin: mp_core_resume (the core keeps the way back for 120 s after a drop).
// - mp_ui_slot_friend_code: the old hook, now the core's own answer (mp_core_slot_friend_code: the service's verified code online, the
//   announced one in a Same Wi-Fi / Nearby room). It was a stub that always said "no code".
// - mp_gather_wifi_connected / mp_gather_nearby_possible: the gathering driver's radio questions (wire: nothing defined them, so the
//   driver always believed in Wi-Fi and never offered Nearby).
// - flow.cfg (first character pick, "Who can join", last co-op file, "Played with") and coop.cfg (who played on each co-op file and
//   where): small text files in <data>/mp, written atomically like mp.cfg (temporary file, verified, previous kept as .bak). They are
//   never save files and never read by the save code.
#include "port/mp_test_env.h"   // worldsync: desktop-only test switches (nothing on the console)
#define MP_UI_FLOW_DEFINES_STRONG 1   // (this unit defines these functions: strong, see port/mp_ui_flow.h)
#include "port/mp_ui_flow.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_config.h"
#include "mp_core.h"
#include "mp_core_sys.h"
#include "mp_netctl_guard.h"
#include "mp_posix_io.h"   // smallfix2: POSIX file calls (the Vita wraps fopen for game data)
#include "mp_savefile.h"   // keepsafe F2: every write through the save layer's atomic writer
#include "port/mp_bridge.h"
#include "port/mp_core_game.h"   // wire: mp_core_slot_friend_code

#if defined(__vita__)
#include <psp2/net/netctl.h>
#pragma weak sceNetCtlInetGetState
#endif

extern "C" bool mp_service_enabled(void) __attribute__((weak));   // platform/src/mp/mp_service.cpp (defined with C linkage: without extern "C" this weak reference never matched it, so the menu always showed "no service")

namespace {

// ---- atomic text files (the mp.cfg pattern) ---------------------------------------------------------------------------------
void FilePath(char* out, size_t cap, const char* name, const char* suffix) { snprintf(out, cap, "%s/%s%s", mp_config_dir(), name, suffix); }

// keepsafe F1-F4 (share/keepsafe-requests.md, docs/MP-DATA-FORMATS.md section 3): a file written by a NEWER game is read for the keys
// this build knows and never rewritten (changes stay in RAM); a damaged file is copied aside (<name>.unreadable, never overwritten)
// before anything is written over it; unknown lines (a newer build's keys, a co-op file number this build does not know) are kept and
// written back; lines of any length are read whole; every write goes through mp_save_write_atomic (<name>.mptmp, <name>.mpbak).
constexpr int kFileVersion = 1;
constexpr uint32_t kFileMax = 32 * 1024;
// optfix item 10: one 32 KB scratch for ReadKeys (flow.cfg, coop.cfg) and WriteAtomic's copy-aside, not one static buffer each (3 x 32 KB of
// bss before). They never overlap on the game thread (the read callbacks do not write); the busy flag makes a second thread that ever got
// here fail cleanly (a read finds "no file", a write returns false and nothing is lost: the data stays in memory) instead of corrupting it.
uint8_t s_fileScratch[kFileMax + 1];
bool s_fileScratchBusy = false;
struct ScratchLease {
    bool ok;
    ScratchLease() : ok(!__atomic_exchange_n(&s_fileScratchBusy, true, __ATOMIC_ACQUIRE)) {}
    ~ScratchLease() { if (ok) __atomic_store_n(&s_fileScratchBusy, false, __ATOMIC_RELEASE); }
    ScratchLease(const ScratchLease&) = delete;
    ScratchLease& operator=(const ScratchLease&) = delete;
};
struct FileState {
    bool found = false;      // the file (or its .mpbak / old .bak) was read
    bool newer = false;      // version > kFileVersion: never written
    bool damaged = false;    // something in it did not parse (a copy is kept aside before the first write)
    bool aside = false;      // that copy exists
    bool full = false;       // the unknown lines did not fit: never written (nothing may be dropped)
    char unknown[4096];      // unknown lines, verbatim, each ending in '\n'
    int unknownLen = 0;
};
void KeepUnknown(FileState& st, const char* line, int len) {
    if (st.unknownLen + len + 1 >= static_cast<int>(sizeof st.unknown)) {
        st.full = true;
        return;
    }
    memcpy(st.unknown + st.unknownLen, line, len);
    st.unknownLen += len;
    st.unknown[st.unknownLen++] = '\n';
    st.unknown[st.unknownLen] = 0;
}

// Reads <dir>/<name> (else its .mpbak, else the old .bak): `fn(key, value, rawLine, rawLen)` for each "key = value" line; fn returns
// false for a key it does not know (the line is kept). Lines that are neither comments nor key = value mark the file damaged.
template < class Fn >
bool ReadKeys(const char* name, FileState& st, Fn fn) {
    ScratchLease lease;
    if (!lease.ok) { mp_log("flow: %s not read (the scratch is in use)", name); return false; }
    uint8_t* const buf = s_fileScratch;
    char path[300];
    uint32_t len = 0;
    const char* suffixes[] = {"", ".mpbak", ".bak"};
    bool got = false;
    for (const char* suf : suffixes) {
        FilePath(path, sizeof path, name, suf);
        const int r = mp_save_read_file(path, buf, kFileMax, &len);
        if (r == 0) { got = true; break; }
        if (r != -ENOENT) {   // there, but unreadable (too big, an I/O error): damaged, try the next
            st.damaged = true;
            mp_log("flow: %s could not be read (%d)", path, r);
        }
    }
    if (!got) return false;
    buf[len] = 0;
    st.found = true;
    char* p = reinterpret_cast<char*>(buf);
    while (*p) {
        char* end = strchr(p, '\n');
        const int rawLen = end ? static_cast<int>(end - p) : static_cast<int>(strlen(p));
        char* next = end ? end + 1 : p + rawLen;
        if (end) *end = 0;
        char raw[512];
        const int keep = rawLen < static_cast<int>(sizeof raw) ? rawLen : static_cast<int>(sizeof raw) - 1;
        memcpy(raw, p, keep);
        raw[keep] = 0;
        char* line = p;
        while (*line == ' ' || *line == '\t') line++;
        char* eq = strchr(line, '=');
        if (line[0] == '#' || line[0] == 0 || line[0] == '\r') {
            // (a comment or an empty line: the writer writes its own header)
        } else if (!eq) {
            st.damaged = true;
        } else {
            *eq = 0;
            char* k = line;
            char* v = eq + 1;
            for (char* e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
            while (*v == ' ') v++;
            for (char* e = v + strlen(v); e > v && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
            if (!strcmp(k, "version")) {
                const int ver = atoi(v);
                if (ver > kFileVersion) st.newer = true;
                if (ver < 1) st.damaged = true;
            } else if (!fn(k, v)) {
                if (rawLen >= static_cast<int>(sizeof raw)) st.full = true;   // (keepsafe F4: read whole, but too long to keep: never written)
                else KeepUnknown(st, raw, keep);
            }
        }
        p = next;
    }
    if (st.newer) mp_log("flow: %s was written by a newer game: read only", name);
    if (st.damaged) mp_log("flow: %s has lines this build cannot read", name);
    return true;
}

// Writes `text` (this build's keys) plus the unknown lines kept, through the save layer's atomic writer. Never over a newer game's
// file; a damaged file is copied to <name>.unreadable first (once; an existing copy is never replaced).
bool WriteAtomic(const char* name, const char* text, FileState& st) {
    if (st.newer || st.full) {
        mp_log("flow: %s not written (%s): kept in memory", name, st.newer ? "a newer game's file" : "unknown lines did not fit");
        return false;
    }
    if (!mp_sys_mkdirs(mp_config_dir())) {
        mp_log("flow: folder %s could not be made", mp_config_dir());
        return false;
    }
    char path[300];
    FilePath(path, sizeof path, name, "");
    if (st.damaged && !st.aside) {
        char aside[300];
        FilePath(aside, sizeof aside, name, ".unreadable");
        ScratchLease lease;
        if (!lease.ok) { mp_log("flow: %s not written (the scratch is in use)", name); return false; }
        uint8_t* const old = s_fileScratch;
        uint32_t len = 0;
        if (mp_io_can_open(aside)) {   // smallfix2: POSIX open, not fopen
            st.aside = true;   // (an earlier copy stays as it is)
        } else if (mp_save_read_file(path, old, kFileMax, &len) == 0) {
            st.aside = mp_save_write_atomic(aside, old, len);
            if (!st.aside) {
                mp_log("flow: %s could not be copied aside: not written", name);
                return false;
            }
            mp_log("flow: %s kept as %s.unreadable", name, name);
        } else {
            st.aside = true;   // (nothing there to keep)
        }
    }
    static char out[8192];
    const int n = snprintf(out, sizeof out, "version = %d\n%s%s", kFileVersion, text, st.unknown);
    if (n <= 0 || n >= static_cast<int>(sizeof out)) return false;
    if (!mp_save_write_atomic(path, reinterpret_cast<const uint8_t*>(out), static_cast<uint32_t>(n))) {
        mp_log("flow: %s could not be written", path);
        return false;
    }
    return true;
}

int Clamp(const char* v, int lo, int hi) {
    const int n = atoi(v);
    return n < lo ? lo : (n > hi ? hi : n);
}

// ---- flow.cfg ----------------------------------------------------------------------------------------------------------------
MpFlowStore s_flow;
bool s_flowLoaded = false, s_flowFound = false;

// lanfix: the Played With names outrank strangers in the Same Wi-Fi room list (mp_lan.h)
void PushTrustedNames() {
    char names[MP_PLAYED_WITH][MP_NAME_BYTES + 1];
    const int n = s_flow.playedCount < MP_PLAYED_WITH ? s_flow.playedCount : MP_PLAYED_WITH;
    for (int i = 0; i < n; i++) memcpy(names[i], s_flow.playedWith[i], sizeof names[i]);
    if (mp_lan_set_trusted_names) mp_lan_set_trusted_names(0, names, n);
}
FileState s_flowFile;

void LoadFlow() {
    if (s_flowLoaded) return;
    s_flowLoaded = true;
    memset(&s_flow, 0, sizeof s_flow);
    s_flowFile = FileState();
    s_flowFound = ReadKeys("flow.cfg", s_flowFile, [](const char* k, const char* v) {
        if (!strcmp(k, "char_picked")) s_flow.charPicked = Clamp(v, 0, 1);
        else if (!strcmp(k, "reach")) s_flow.reach = (uint8_t)Clamp(v, 0, MP_REACH_COUNT - 1);
        else if (!strcmp(k, "last_coop")) s_flow.lastCoop = (uint8_t)Clamp(v, 0, 6);
        else if (!strcmp(k, "falls_asked")) s_flow.fallsAsked = (uint8_t)Clamp(v, 0, 1);
        else if (!strcmp(k, "played")) {   // newest first; "played_code" after a name is that player's friend code
            if (s_flow.playedCount < MP_PLAYED_WITH && *v) {
                char* dst = s_flow.playedWith[s_flow.playedCount];
                mp_name_clean(v, strlen(v), dst);
                if (dst[0]) {
                    s_flow.playedCode[s_flow.playedCount][0] = 0;
                    s_flow.playedCount++;   // (core2: nothing usable left: not kept)
                }
            }
        } else if (!strcmp(k, "played_code")) {
            if (s_flow.playedCount > 0 && strlen(v) < sizeof s_flow.playedCode[0]) snprintf(s_flow.playedCode[s_flow.playedCount - 1], sizeof s_flow.playedCode[0], "%s", v);
        } else {
            return false;
        }
        return true;
    });
    PushTrustedNames();   // lanfix
}

// ---- coop.cfg ----------------------------------------------------------------------------------------------------------------
MpCoopMeta s_coop[6];
bool s_coopLoaded = false;

FileState s_coopFile;

void LoadCoop() {
    if (s_coopLoaded) return;
    s_coopLoaded = true;
    memset(s_coop, 0, sizeof s_coop);
    for (MpCoopMeta& m : s_coop) m.galaxy = -1;
    s_coopFile = FileState();
    static int slot;      // the section being read: 1..6, 0 none yet, -1 a file number this build does not know (kept verbatim)
    static FileState* st;
    slot = 0;
    st = &s_coopFile;
    ReadKeys("coop.cfg", s_coopFile, [](const char* k, const char* v) {
        if (!strcmp(k, "file")) {
            const int f = atoi(v);
            slot = f >= 1 && f <= 6 ? f : -1;   // (keepsafe F3: another file number is a newer build's: kept, never deleted)
            if (slot >= 1) s_coop[slot - 1].used = 1;
            return slot >= 1;
        }
        if (slot == 0) {   // (a key before any section: kept in a section of its own, never under a file it is not about)
            KeepUnknown(*st, "file = 0", 8);
            slot = -1;
            return false;
        }
        if (slot < 0) return false;   // (the lines of an unknown section: kept verbatim)
        if (!strcmp(k, "galaxy")) {
            s_coop[slot - 1].galaxy = (int8_t)Clamp(v, -1, 120);
            return true;
        }
        if (!strcmp(k, "player")) {   // "<look> <name>"
            MpCoopMeta& m = s_coop[slot - 1];
            const char* name = strchr(v, ' ');
            if (m.count >= 4 || !name || !name[1]) return false;
            const int look = Clamp(v, 0, MP_LOOK_COUNT - 1);
            m.looks[m.count] = (uint8_t)look;
            mp_name_clean(name + 1, strlen(name + 1), m.names[m.count]);
            mp_name_or_character(m.names[m.count], look);   // core2
            m.count++;
            return true;
        }
        // an unknown key inside a known section: kept under its own "file = N" so it goes back where it was
        char tagged[16];
        const int n = snprintf(tagged, sizeof tagged, "file = %d", slot);
        KeepUnknown(*st, tagged, n);
        return false;
    });
}

}  // namespace

extern "C" {

void mp_ui_net_info(MpNetInfo* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->wifiUp = true;
#if defined(__vita__)
    // (asked directly: the core's watch only runs during a session. An error of the call itself, e.g. the network control layer is
    // not started yet, says nothing: Wi-Fi is assumed; and with the NET module not loaded yet the call is not made at all (mpcrash32: its stub jumps to 0), and the Join list offers Nearby with one row)
    int state = 0;
    // (the Multiplayer menu is about to use the network anyway: load the NET module here, once, so the Wi-Fi state is real; loading is idempotent)
    // (rev32 F3: one try only, or a refusal is repeated every call - NetInfo runs several times a frame; and a refusal because another thread's
    // load of the same module is in flight is no failure: the module is then loaded, which is what the next line tests)
    static bool loadTried = false;
    if (!mp_netctl_live() && !loadTried) {
        loadTried = true;
        if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0 && !mp_netctl_live()) mp_log("ui: NET module not loaded; Wi-Fi assumed up");
    }
    if (mp_netctl_live() && sceNetCtlInetGetState && sceNetCtlInetGetState(&state) >= 0) out->wifiUp = state == SCE_NETCTL_STATE_CONNECTED;
#endif
    if (const char* e = MP_TEST_GETENV("SMG_MP_TEST_NO_WIFI")) if (*e == '1') out->wifiUp = false;   // (desktop tests: the no-Wi-Fi list)
    out->service = mp_service_enabled && mp_service_enabled();
    MpProfile p;
    memset(&p, 0, sizeof p);
    mp_ui_profile(&p);
    out->registered = p.registered;
}

bool mp_ui_rejoin(void) {
    const bool ok = mp_core_resume();
    mp_log("ui: rejoin %s", ok ? "started" : "not possible");
    return ok;
}

bool mp_ui_slot_friend_code(int slot, char* out) {
    // wire: the core knows the code (core2). out is 10 bytes ("1234-5678" + NUL), as the header says.
    if (out) out[0] = 0;
    return out && mp_core_slot_friend_code(slot, out, 10);
}

// ---- the gathering driver's radio questions (platform/src/mp/mp_gather.cpp asks; weak there) ------------------------------------
// Wi-Fi: connected to an access point with an address (the Join list's question: one answer for both). Nearby (ad hoc): the Vita has
// it (mp_net_link_poll); the desktop build's only link is Wi-Fi UDP, so there it is off unless a test asks (SMG_MP_TEST_NEARBY=1).
bool mp_gather_wifi_connected(void) {
    MpNetInfo n;
    mp_ui_net_info(&n);
    return n.wifiUp;
}
bool mp_gather_nearby_possible(void) {
#if defined(__vita__)
    return true;
#else
    const char* e = getenv("SMG_MP_TEST_NEARBY");
    return e && *e == '1';
#endif
}

bool mp_flow_store_get(MpFlowStore* out) {
    LoadFlow();
    if (out) *out = s_flow;
    return s_flowFound;
}

void mp_flow_store_set(const MpFlowStore* in) {
    if (!in) return;
    LoadFlow();
    MpFlowStore c = *in;
    if (c.reach >= MP_REACH_COUNT) c.reach = MP_REACH_LOCAL;
    if (c.lastCoop > 6) c.lastCoop = 0;
    if (c.playedCount > MP_PLAYED_WITH) c.playedCount = MP_PLAYED_WITH;
    for (int i = 0; i < c.playedCount; i++) {
        char clean[MP_NAME_BYTES + 1];
        c.playedWith[i][MP_NAME_BYTES] = 0;
        mp_name_clean(c.playedWith[i], strlen(c.playedWith[i]), clean);
        memcpy(c.playedWith[i], clean, sizeof clean);
        c.playedCode[i][sizeof c.playedCode[i] - 1] = 0;
        for (char* q = c.playedCode[i]; *q; q++)   // (a code is letters, digits and dashes: nothing that could break a line)
            if (!((*q >= '0' && *q <= '9') || (*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || *q == '-')) { c.playedCode[i][0] = 0; break; }
    }
    char text[4096];
    int n = snprintf(text, sizeof text, "# Super Mario Galaxy Vita: what the multiplayer menus remember (written by the game)\n"
                                        "char_picked = %d\nreach = %d\nlast_coop = %d\nfalls_asked = %d\n", c.charPicked ? 1 : 0, c.reach, c.lastCoop,
                                        c.fallsAsked ? 1 : 0);
    for (int i = 0; i < c.playedCount && n > 0 && n < (int)sizeof text; i++) {
        n += snprintf(text + n, sizeof text - n, "played = %s\n", c.playedWith[i]);
        if (c.playedCode[i][0] && n > 0 && n < (int)sizeof text) n += snprintf(text + n, sizeof text - n, "played_code = %s\n", c.playedCode[i]);
    }
    if (n <= 0 || n >= (int)sizeof text) return;
    s_flow = c;
    s_flowFound = true;
    PushTrustedNames();   // lanfix
    if (WriteAtomic("flow.cfg", text, s_flowFile)) mp_log("flow: saved flow.cfg (played=%d reach=%d)", c.playedCount, c.reach);
}

bool mp_coop_meta_get(int slot, MpCoopMeta* out) {
    if (slot < 1 || slot > 6 || !out) return false;
    LoadCoop();
    *out = s_coop[slot - 1];
    return out->used || out->count > 0 || out->galaxy >= 0;
}

void mp_coop_meta_set(int slot, const MpCoopMeta* in) {
    if (slot < 1 || slot > 6 || !in) return;
    LoadCoop();
    MpCoopMeta m = *in;
    if (m.count > 4) m.count = 4;
    m.used = 1;   // (every write is a session played on it or progress kept in it)
    for (int i = 0; i < m.count; i++) {
        char clean[MP_NAME_BYTES + 1];
        m.names[i][MP_NAME_BYTES] = 0;
        mp_name_clean(m.names[i], strlen(m.names[i]), clean);
        memcpy(m.names[i], clean, sizeof clean);
        if (m.looks[i] >= MP_LOOK_COUNT) m.looks[i] = 0;
        mp_name_or_character(m.names[i], m.looks[i]);   // core2
    }
    s_coop[slot - 1] = m;
    char text[4096];
    int n = snprintf(text, sizeof text, "# Super Mario Galaxy Vita: who played on each co-op file (written by the game; not a save file)\n");
    for (int s = 0; s < 6 && n > 0 && n < (int)sizeof text; s++) {
        const MpCoopMeta& c = s_coop[s];
        if (!c.used && c.count == 0 && c.galaxy < 0) continue;
        n += snprintf(text + n, sizeof text - n, "file = %d\ngalaxy = %d\n", s + 1, c.galaxy);
        for (int i = 0; i < c.count && n > 0 && n < (int)sizeof text; i++) n += snprintf(text + n, sizeof text - n, "player = %d %s\n", c.looks[i], c.names[i]);
    }
    if (n <= 0 || n >= (int)sizeof text) return;
    if (WriteAtomic("coop.cfg", text, s_coopFile)) mp_log("flow: co-op file %d players=%d galaxy=%d", slot, m.count, m.galaxy);
}

// MP3 keepfix (keepsafe review 4): an import / restore / rescue replaced flow.cfg / coop.cfg: the next read loads them again (registered as a
// keepsafe reload hook by mp_port_boot). Nothing is written here.
// MP3 keepfix 3: keepsafe's "Bring back settings": flow.cfg / coop.cfg replaced through this file's own writer (.tmp, read back, .bak), the
// cached copy dropped. Only those two names. (menuflow: the writer is the save layer's atomic one, as the file's own writes.)
bool mp_ui_flow_replace_file(const char* name, const char* text) {
    if (!name || !text || (strcmp(name, "flow.cfg") != 0 && strcmp(name, "coop.cfg") != 0)) return false;
    // (the copy's whole file, version line and unknown lines included, written as it is through the save layer's atomic writer: the
    // keepsafe module has already made a "before" copy and checked what it brings back)
    if (!mp_sys_mkdirs(mp_config_dir())) return false;
    char path[300];
    FilePath(path, sizeof path, name, "");
    if (!mp_save_write_atomic(path, reinterpret_cast<const uint8_t*>(text), static_cast<uint32_t>(strlen(text)))) return false;
    s_flowLoaded = false;
    s_coopLoaded = false;
    return true;
}

void mp_ui_flow_forget(void) {
    s_flowLoaded = false;
    s_coopLoaded = false;
}

}  // extern "C"
