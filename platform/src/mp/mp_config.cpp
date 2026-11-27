// Galaxy 3.0 multiplayer (port side, owner: core): mp.cfg. See mp_config.h.
extern "C" void port_host_alloc_scope_enter(void) __attribute__((weak));   // (as mp_core.cpp: the game thread's file work uses the host allocator)
extern "C" void port_host_alloc_scope_leave(void) __attribute__((weak));
namespace {
struct MpCfgHostAllocScope {
    MpCfgHostAllocScope() { if (port_host_alloc_scope_enter) port_host_alloc_scope_enter(); }
    ~MpCfgHostAllocScope() { if (port_host_alloc_scope_leave) port_host_alloc_scope_leave(); }
};
}  // namespace

#include "mp_name_filter.h"
#include "mp_config.h"

#include "mp_core_sys.h"
#include "mp_posix_io.h"   // netfix B8b
#include "port/mp_chat.h"

#include <atomic>
#include <string>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

// ingame: the console's user name (net/np_glue.h: NpLanPlayerName, the Vita's SCE_SYSTEM_PARAM_ID_USERNAME; weak: absent in host tests)
extern "C" const char* NpLanPlayerName(void) __attribute__((weak));

namespace {

MpConfig s_cfg;
bool s_init = false;
std::atomic<bool> s_entryDirty{false};   // (atomic: mp_config_save also runs on the helper thread) the entry screen's choice changed in RAM and is not in mp.cfg yet (mp_entry_remember(.., false))
char s_dir[256] = {0};

// savefix (review finding 8): what the loaded file said, so a write never loses what the player has.
//  - s_raw: every "key = value" line of the file that loaded (keys this build does not know, values it clamps, all of them);
//  - s_loadedFmt: the value of each key this build writes, as it would write it right after the load;
// on save a known key whose value is still what was loaded is written with the file's own text (a value from a newer build
// survives), and the keys this build does not know are written back as they were.
//  - an mp.cfg that is there but cannot be read / parsed (another layout: a newer build; or damage) is never written over: this
//    run works from memory (s_readOnly, owner's rule: nothing ends up overwritten by an update; keepsafe K1/K2). It stays read
//    only until the file parses again or is removed / reset by an explicit player action (keepsafe's restore, a future menu item).
struct RawLine { char key[32]; char val[160]; };
const int kRawMax = 48;
RawLine s_raw[kRawMax];
int s_rawCount = 0;
RawLine s_loadedFmt[kRawMax];
int s_loadedCount = 0;
bool s_readOnly = false;     // keepsafe K1/K2/K4: mp.cfg is not understood (another layout, version > 1, damage): never write it
int s_lastVersion = 0;

bool KeySet(const uint8_t k[16]) {
    for (int i = 0; i < 16; i++)
        if (k[i]) return true;
    return false;
}

const char* SystemName() { return NpLanPlayerName ? NpLanPlayerName() : nullptr; }

void Defaults(MpConfig* c) {
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "Mario");   // (nameTyped 0: Clamp keeps it the character's name)
    c->look = MP_LOOK_MARIO;
    c->conn = MP_CONN_WIFI;
    c->mode = MP_MODE_TOGETHER;
    c->maxPlayers = 4;
    c->interaction = 1;
    c->sharedKills = 1;
    c->nameTags = 1;
    c->rivals = 1;
    c->raceWaitSec = 18;
    c->visibility = 0;
    c->relayOnly = 0;
    c->dropIn = 1;
    c->powerSaveTipShown = 0;
    c->downed = MP_DOWNED_BUBBLES;
    c->stun = 1;
    c->entry = 0;   // entry: Single Player
}

void Clamp(MpConfig* c) {
    char clean[MP_NAME_BYTES + 1];
    mp_name_clean(c->name, sizeof c->name, clean);
    memcpy(c->name, clean, sizeof clean);
    if (c->look >= MP_LOOK_COUNT) c->look = MP_LOOK_MARIO;
    if (c->nameTyped > 1) c->nameTyped = 1;
    if (!c->name[0]) c->nameTyped = 0;
    if (!c->nameTyped) mp_default_name(c->look, c->name);   // 13 Nov: until the player types a name, it is the character's (follows a change of character)
    if (c->conn < MP_CONN_ONLINE || c->conn > MP_CONN_NEARBY) c->conn = MP_CONN_WIFI;
    if (c->mode != MP_MODE_TOGETHER && c->mode != MP_MODE_RACE) c->mode = MP_MODE_TOGETHER;
    if (c->maxPlayers < 2 || c->maxPlayers > MP_MAX_PLAYERS) c->maxPlayers = MP_MAX_PLAYERS;
    if (c->interaction > 2) c->interaction = 1;
    if (c->sharedKills > 1) c->sharedKills = 1;
    if (c->nameTags > 2) c->nameTags = 1;
    if (c->rivals > 2) c->rivals = 1;
    if (c->raceWaitSec > 60) c->raceWaitSec = 18;
    if (c->visibility > 2) c->visibility = 0;
    if (c->relayOnly > 1) c->relayOnly = 0;
    if (c->dropIn > 1) c->dropIn = 1;
    if (c->powerSaveTipShown > 1) c->powerSaveTipShown = 1;
    if (c->downed >= MP_DOWNED_COUNT) c->downed = MP_DOWNED_BUBBLES;
    c->server[MP_SERVER_URL_MAX] = 0;
    char url[MP_SERVER_URL_MAX + 1];
    if (!mp_server_url_clean(c->server, url, sizeof url)) c->server[0] = 0;
    else memcpy(c->server, url, sizeof url);
    if (c->stun > 1) c->stun = 1;
    if (c->entry > 1) c->entry = 0;
    if (c->rulesHide > 1) c->rulesHide = 1;
}

void Init() {
    if (s_init) return;
    s_init = true;
    Defaults(&s_cfg);
    mp_default_name(s_cfg.look, s_cfg.name);   // 13 Nov: the character's name, never the console's user name
}

void EnvOverrides() {
    if (const char* n = getenv("SMG_MP_NAME"))
        if (*n) {
            mp_name_clean(n, strlen(n), s_cfg.name);
            if (s_cfg.name[0]) s_cfg.nameTyped = 1;
        }
    if (const char* l = getenv("SMG_MP_LOOK"))
        if (*l) {
            const int v = atoi(l);
            if (v >= 0 && v < MP_LOOK_COUNT) s_cfg.look = (uint8_t)v;
        }
    if (!s_cfg.nameTyped) mp_default_name(s_cfg.look, s_cfg.name);   // (13 Nov: the character's name follows a look set here)
    if (const char* d = getenv("SMG_MP_DOWNED"))
        if (*d) {
            const int v = atoi(d);
            if (v >= 0 && v < MP_DOWNED_COUNT) s_cfg.downed = (uint8_t)v;
        }
}

void Path(char* out, size_t cap, const char* suffix) { snprintf(out, cap, "%s/mp.cfg%s", mp_config_dir(), suffix); }

bool ParseFile(const char* path, MpConfig* c, RawLine* raw = nullptr, int* rawCount = nullptr) {
    // netfix B8b: the file is read whole with POSIX calls (an mp.cfg is a few hundred bytes; 32 KB is far above any real one)
    // savesus S7: ONE static buffer, not thread_local (the Vita's emulated TLS gave every thread that parsed a config its own 32 KB, never freed).
    // Every caller holds CfgMu() (load, save, record_upnp, replace_held through mp_config_hold), so the buffer is never used by two threads at once.
    static char text[32 * 1024 + 1];
    const long got = mp_io_read(path, text, sizeof text - 1);
    if (got < 0) return false;
    text[got] = 0;
    char line[256];
    bool any = false;
    if (rawCount) *rawCount = 0;
    const char* cursor = text;
    for (;;) {
        if (!*cursor) break;
        {   // (as fgets: a longer line comes as several pieces)
            size_t n = 0;
            while (cursor[n] && cursor[n] != '\n' && n < sizeof line - 1) n++;
            if (cursor[n] == '\n' && n < sizeof line - 1) n++;
            memcpy(line, cursor, n);
            line[n] = 0;
            cursor += n;
        }
        char* eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        char* k = line;
        char* v = eq + 1;
        *eq = 0;
        while (*k == ' ' || *k == '\t') k++;
        for (char* e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (char* e = v + strlen(v); e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        if (raw && rawCount && *rawCount < kRawMax && *k && strlen(k) < sizeof raw[0].key && strlen(v) < sizeof raw[0].val) {
            int i = 0;
            while (i < *rawCount && strcmp(raw[i].key, k) != 0) i++;   // (a key twice: the last one, as the parser reads it)
            snprintf(raw[i].key, sizeof raw[i].key, "%s", k);
            snprintf(raw[i].val, sizeof raw[i].val, "%s", v);
            if (i == *rawCount) (*rawCount)++;
        }
        const int n = atoi(v);
        const uint8_t b = (uint8_t)(n < 0 ? 255 : n > 255 ? 255 : n);
        if (!strcmp(k, "version")) {
            any = true;
            s_lastVersion = atoi(v);
        }
        else if (!strcmp(k, "name")) mp_name_clean(v, strlen(v), c->name);
        else if (!strcmp(k, "name_typed")) c->nameTyped = b ? 1 : 0;
        else if (!strcmp(k, "look")) c->look = b;
        else if (!strcmp(k, "connection")) c->conn = b;
        else if (!strcmp(k, "mode")) c->mode = b;
        else if (!strcmp(k, "max_players")) c->maxPlayers = b;
        else if (!strcmp(k, "interaction")) c->interaction = b;
        else if (!strcmp(k, "shared_kills")) c->sharedKills = b;
        else if (!strcmp(k, "name_tags")) c->nameTags = b;
        else if (!strcmp(k, "rivals")) c->rivals = b;
        else if (!strcmp(k, "race_wait")) c->raceWaitSec = b;
        else if (!strcmp(k, "visibility")) c->visibility = b;
        else if (!strcmp(k, "relay_only")) c->relayOnly = b;
        else if (!strcmp(k, "drop_in")) c->dropIn = b;
        else if (!strcmp(k, "power_save_tip")) c->powerSaveTipShown = b;
        else if (!strcmp(k, "downed")) c->downed = b;
        else if (!strcmp(k, "server")) { if (!mp_server_url_clean(v, c->server, sizeof c->server)) c->server[0] = 0; }   // serverfix
        else if (!strcmp(k, "stun")) c->stun = b ? 1 : 0;   // corefix 6
        else if (!strcmp(k, "entry")) c->entry = b;   // entry
        else if (!strcmp(k, "upnp_permanent")) { const long pv = atol(v); c->upnpPermanent = (uint16_t)(pv > 0 && pv < 65536 ? pv : 0); }   // corefix 8
        else if (!strcmp(k, "console_key")) {   // corefix 6: 32 hex digits, else ignored (a new key is made)
            uint8_t key[16];
            bool ok = strlen(v) == 32;
            for (int i = 0; ok && i < 16; i++) {
                unsigned x = 0;
                for (int j = 0; j < 2; j++) {
                    const char ch = v[i * 2 + j];
                    const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
                    if (d < 0) ok = false;
                    else x = x * 16 + (unsigned)d;
                }
                key[i] = (uint8_t)x;
            }
            if (ok) memcpy(c->consoleKey, key, 16);
        }
        else if (!strcmp(k, "rules_hide")) c->rulesHide = b > 1 ? 1 : b;   // touches
        else if (!strcmp(k, "rules_seen")) {   // touches: up to four hex words, comma separated
            const char* q = v;
            for (int i = 0; i < 4; i++) {
                char* end = 0;
                c->rulesSeen[i] = (uint32_t)strtoul(q, &end, 16);
                if (!end || end == q) { c->rulesSeen[i] = 0; break; }
                q = *end == ',' ? end + 1 : end;
            }
        }
    }
    return any;
}

}  // namespace

bool mp_server_url_clean(const char* in, char* out, size_t cap) {
    if (out && cap) out[0] = 0;
    if (!in || !out || cap < MP_SERVER_URL_MAX + 1) return false;
    while (*in == ' ' || *in == '\t') in++;
    char t[MP_SERVER_URL_MAX + 2];
    size_t n = 0;
    for (; in[n]; n++) {
        if (n >= MP_SERVER_URL_MAX + 1) return false;   // too long
        t[n] = in[n];
    }
    while (n && (t[n - 1] == ' ' || t[n - 1] == '\t' || t[n - 1] == '\r' || t[n - 1] == '\n')) n--;
    if (n && t[n - 1] == '/') n--;                       // one trailing slash is fine
    t[n] = 0;
    if (n > MP_SERVER_URL_MAX || n < 12 || strncmp(t, "https://", 8) != 0) return false;
    // host: letters, digits, '-' and '.', labels not empty and not starting / ending with '-', at least one dot
    const char* h = t + 8;
    size_t hl = 0, labels = 1;
    bool labelStart = true;
    for (; h[hl] && h[hl] != ':'; hl++) {
        const char ch = h[hl];
        const bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
        if (ch == '.') {
            if (labelStart) return false;
            labelStart = true;
            labels++;
            if (t[8 + hl + 1] == '.') return false;
        } else if (alnum) {
            labelStart = false;
        } else if (ch == '-') {
            if (labelStart || h[hl + 1] == '.' || h[hl + 1] == ':' || !h[hl + 1]) return false;
        } else {
            return false;   // '@', '/', '?', '#', spaces, control characters, '[': no user info, path, query, IPv6 literal
        }
    }
    if (hl == 0 || hl > 100 || labelStart || labels < 2) return false;
    if (h[hl] == ':') {
        const char* p = h + hl + 1;
        size_t d = 0;
        unsigned port = 0;
        for (; p[d]; d++) {
            if (p[d] < '0' || p[d] > '9' || d >= 5) return false;
            port = port * 10 + unsigned(p[d] - '0');
        }
        if (d == 0 || port == 0 || port > 65535) return false;
    }
    for (size_t i = 8; i < n && t[i] != ':'; i++) if (t[i] >= 'A' && t[i] <= 'Z') t[i] = char(t[i] - 'A' + 'a');   // host names are case-insensitive
    memcpy(out, t, n + 1);
    return true;
}

void mp_name_or_character(char* name, int look) {
    static const char* const kChar[MP_LOOK_COUNT] = {"Mario", "Luigi", "Toad", "Cosmic Mario", "Yellow Toad", "Cosmic Luigi"};
    if (name && !name[0]) snprintf(name, MP_NAME_BYTES + 1, "%s", kChar[look >= 0 && look < MP_LOOK_COUNT ? look : 0]);
}

void mp_name_clean(const char* in, size_t len, char* out) {
    // corefix 7: the chat character rules (port/mp_chat.h mp_chat_char_ok) apply to names too: no private-use characters (the panels'
    // picture markup), no bidi overrides / zero-width / format characters, nothing outside the BMP; invalid UTF-8 and control
    // characters become '?', runs of spaces collapse, no edge spaces; at most MP_NAME_BYTES bytes, never splitting a character.
    size_t o = 0, i = 0;
    bool pendingSpace = false;
    const uint8_t* s = (const uint8_t*)in;
    while (in && i < len && s[i]) {
        uint32_t c = mp_chat_utf8_next(s, len, &i);
        if (c == 0xFFFFFFFFu || c < 0x20 || (c >= 0x7F && c < 0xA0) || (c >= 0x10000 && c <= 0x10FFFF) || (c >= 0xE000 && c <= 0xF8FF)) c = '?';
        if (c == 0x3000 || c == 0xA0 || c == ' ') {
            pendingSpace = o > 0;
            continue;
        }
        if (!mp_chat_char_ok(c)) continue;   // (zero-width, bidi, specials: dropped)
        const size_t el = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
        if (o + (pendingSpace ? 1 : 0) + el > MP_NAME_BYTES) break;
        if (pendingSpace) {
            out[o++] = ' ';
            pendingSpace = false;
        }
        if (el == 1) {
            out[o++] = (char)c;
        } else if (el == 2) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    if (o == 0) {   // core2: nothing usable left: empty (the caller shows the character's name: mp_name_or_character; never "Player")
        out[0] = 0;
        return;
    }
    out[o] = 0;
    // The name filter (mp_name_filter.h, the same list and rules as the service): a name that is refused is shown as the character's name
    // (the callers' mp_name_or_character), never as an error. It matters most for names that come from other consoles on a local Wi-Fi.
    if (mp_nf::Reason(out, o) != mp_nf::OK) out[0] = 0;
}

void mp_default_name(int look, char* out) {
    static const char* const kChar[MP_LOOK_COUNT] = {"Mario", "Luigi", "Toad", "Cosmic Mario", "Yellow Toad", "Cosmic Luigi"};
    const char* n = kChar[look >= 0 && look < MP_LOOK_COUNT ? look : 0];
    snprintf(out, MP_NAME_BYTES + 1, "%s", n);
}

const char* mp_config_dir(void) {
    if (!s_dir[0]) {
#ifdef __vita__
        snprintf(s_dir, sizeof s_dir, "ux0:data/smg/mp");
#else
        const char* d = getenv("PETARI_DATA_DIR");
        snprintf(s_dir, sizeof s_dir, "%s/mp", d && *d ? d : ".");
#endif
    }
    return s_dir;
}

void mp_config_set_dir(const char* dir) { snprintf(s_dir, sizeof s_dir, "%s", dir ? dir : ""); }

const MpConfig* mp_config(void) {
    Init();
    return &s_cfg;
}

// (coordinator review of corefix 8) mp.cfg is changed from two threads: the game thread (name, look, settings: copy, edit, set,
// save) and the network helper thread (the port mapper's upnp_permanent record). One lock covers set and save, and the
// helper's field is not part of what mp_config_set copies, so a game-thread copy made before the record cannot undo it.
static MpMutex& CfgMu() {
    static MpMutex m;
    return m;
}

void mp_config_set(const MpConfig* c) {
    Init();
    if (!c) return;
    MpLock l(CfgMu());
    const uint16_t keep = s_cfg.upnpPermanent;
    s_cfg = *c;
    s_cfg.upnpPermanent = keep;
    s_cfg.name[MP_NAME_BYTES] = 0;
    Clamp(&s_cfg);
}

bool mp_config_record_upnp_permanent(uint16_t port) {
    Init();
    {
        MpLock l(CfgMu());
        if (s_cfg.upnpPermanent == port) return true;
        s_cfg.upnpPermanent = port;
    }
    return mp_config_save();
}

// The keys this build writes and their text (one function for the save and for what the load gave).
template < class F > void Format(const MpConfig& c, F put) {
    char v[64];
    auto num = [&](const char* k, int n) { snprintf(v, sizeof v, "%d", n); put(k, v); };
    put("version", "1");
    put("name", c.name);
    num("name_typed", c.nameTyped ? 1 : 0);
    num("look", c.look);
    num("connection", c.conn);
    num("mode", c.mode);
    num("max_players", c.maxPlayers);
    num("interaction", c.interaction);
    num("shared_kills", c.sharedKills);
    num("name_tags", c.nameTags);
    num("rivals", c.rivals);
    num("race_wait", c.raceWaitSec);
    num("visibility", c.visibility);
    num("relay_only", c.relayOnly);
    num("drop_in", c.dropIn);
    num("power_save_tip", c.powerSaveTipShown);
    num("downed", c.downed);
    num("entry", c.entry);
    if (c.server[0]) put("server", c.server);   // serverfix (already cleaned by Clamp)
    num("stun", c.stun ? 1 : 0);   // corefix 6
    if (c.upnpPermanent) num("upnp_permanent", c.upnpPermanent);   // corefix 8
    if (KeySet(c.consoleKey)) {
        for (int i = 0; i < 16; i++) snprintf(v + i * 2, 3, "%02x", (unsigned)c.consoleKey[i]);
        put("console_key", v);
    }
    num("rules_hide", c.rulesHide ? 1 : 0);   // touches
    snprintf(v, sizeof v, "%x,%x,%x,%x", (unsigned)c.rulesSeen[0], (unsigned)c.rulesSeen[1], (unsigned)c.rulesSeen[2], (unsigned)c.rulesSeen[3]);
    put("rules_seen", v);
}

const RawLine* FindLine(const RawLine* lines, int n, const char* key) {
    for (int i = 0; i < n; i++)
        if (!strcmp(lines[i].key, key)) return &lines[i];
    return nullptr;
}

bool Exists(const char* path) {
    if (!mp_io_can_open(path)) return errno != ENOENT;   // (a file that cannot be opened for another reason is there: never written over)
    return true;
}

bool mp_config_load(void) {
    Init();
    // keepfix (keepsafe review 14): the load rewrites s_cfg, s_raw and s_loadedFmt, which the helper thread's mp_config_save reads: the
    // same lock as the save (keepsafe calls this after an import while the helper may be recording a port mapping)
    MpLock l(CfgMu());
    return mp_config_load_held();
}

// keepfix 2 (keepsafe review 2 #6): the load for a caller that already holds the lock (mp_config_hold(true)): keepsafe reloads the settings
// it just replaced BEFORE it releases the lock, so a helper-thread save waiting on it writes these settings, not the ones from before.
bool mp_config_load_held(void) {
    Init();
    MpConfig c;
    Defaults(&c);
    char path[300];
    Path(path, sizeof path, "");
    s_lastVersion = 0;
    bool ok = ParseFile(path, &c, s_raw, &s_rawCount);
    const bool primaryBad = !ok && Exists(path);
    if (!ok) {
        Defaults(&c);
        Path(path, sizeof path, ".bak");
        ok = ParseFile(path, &c, s_raw, &s_rawCount);
        if (ok) mp_log("config: recovered %s", path);
    }
    s_readOnly = primaryBad || (ok && s_lastVersion > 1);
    if (primaryBad) {
        // keepsafe K1/K2: a copy of the file not understood, as mp.cfg.unreadable (made once, never overwritten)
        char from[300], to[300];
        Path(from, sizeof from, "");
        Path(to, sizeof to, ".unreadable");
        if (!Exists(to)) {
            // netfix B8b: POSIX calls (no fopen from this thread beside the game's)
            const int in = open(from, O_RDONLY | O_CLOEXEC);
            const int out = in >= 0 ? open(to, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0660) : -1;
            char buf[512];
            bool good = in >= 0 && out >= 0;
            while (good) {
                const ssize_t n = read(in, buf, sizeof buf);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) { good = false; break; }
                if (n == 0) break;
                for (ssize_t at = 0; at < n && good;) {
                    const ssize_t w = write(out, buf + at, (size_t)(n - at));
                    if (w < 0 && errno == EINTR) continue;
                    if (w <= 0) good = false;
                    else at += w;
                }
            }
            if (in >= 0) close(in);
            if (out >= 0 && close(out) != 0) good = false;
            if (out >= 0 && !good) remove(to);
        }
    }
    if (s_readOnly)
        mp_log("config: mp.cfg %s: read only, changes stay for this run",
               primaryBad ? "could not be read (another layout or damaged)" : "is from a newer version");
    if (!ok) {
        Defaults(&c);
        s_rawCount = 0;
    }
    // 13 Nov: a file from before "name_typed" (it had the console's user name as the default, or "Player"): a name that is the console's
    // own user name, "Player" or a character's name was never typed by the player: it becomes the character's name (the user name is
    // only compared here, never shown). Any other name was set by the player in mp.cfg: kept (name_typed = 1).
    if (!FindLine(s_raw, s_rawCount, "name_typed")) {
        char sys[MP_NAME_BYTES + 1] = {};
        if (const char* sn = SystemName()) mp_name_clean(sn, strlen(sn), sys);
        bool dflt = !c.name[0] || !strcmp(c.name, "Player") || (sys[0] && !strcmp(c.name, sys));
        for (int l = 0; l < MP_LOOK_COUNT && !dflt; l++) {
            char cn[MP_NAME_BYTES + 1];
            mp_default_name(l, cn);
            dflt = !strcmp(c.name, cn);
        }
        c.nameTyped = dflt ? 0 : 1;
        Clamp(&c);
    }
    Clamp(&c);
    s_loadedCount = 0;
    Format(c, [&](const char* k, const char* v) {
        if (s_loadedCount < kRawMax) {
            snprintf(s_loadedFmt[s_loadedCount].key, sizeof s_loadedFmt[0].key, "%s", k);
            snprintf(s_loadedFmt[s_loadedCount].val, sizeof s_loadedFmt[0].val, "%s", v);
            s_loadedCount++;
        }
    });
    s_cfg = c;
    EnvOverrides();
    mp_log("config: name=%s look=%d conn=%d%s", s_cfg.name, s_cfg.look, s_cfg.conn, ok ? "" : " (defaults)");
    return ok;
}

// keepfix: keepsafe holds the settings lock while it replaces files (mp.cfg among them), so a helper-thread save waits. Not recursive:
// never call mp_config_load / _save / _set between hold(true) and hold(false) on the same thread.
void mp_config_hold(bool hold) {
    if (hold) CfgMu().lock();
    else CfgMu().unlock();
}

// keepfix 3 (keepsafe: "Bring back settings" from this console's own copy): the file's text replaced through this writer's own steps
// (.tmp, read back by the parser, the previous file kept as .bak, rename), then loaded. The caller holds the lock (mp_config_hold(true)).
// Refuses a read-only run (a newer or unreadable mp.cfg is never written over) and text that does not parse.
bool mp_config_replace_held(const char* text, size_t len) {
    Init();
    // keepfix 5: an mp.cfg that could not be READ (damaged) is exactly what a restore is for: allowed; one written by a newer game is never replaced
    if (!text || len == 0 || (s_readOnly && s_lastVersion > 1) || !mp_sys_mkdirs(mp_config_dir())) return false;
    char path[300], tmp[300], bak[300];
    Path(path, sizeof path, "");
    Path(tmp, sizeof tmp, ".tmp");
    Path(bak, sizeof bak, ".bak");
    const bool written = mp_io_write_sync(tmp, text, len);   // (savebug: POSIX calls, synced before the rename)
    const bool closed = true;
    MpConfig verify;
    Defaults(&verify);
    if (!written || !closed || !ParseFile(tmp, &verify)) {
        remove(tmp);
        return false;
    }
    const bool hadPrimary = Exists(path);
    if (hadPrimary) {
        remove(bak);
        rename(path, bak);
    }
    if (rename(tmp, path) != 0) {
        if (hadPrimary) rename(bak, path);
        remove(tmp);
        return false;
    }
    mp_log("config: replaced from a copy (keepsafe)");
    return mp_config_load_held();
}

int mp_config_read_only(void) {
    Init();
    if (!s_readOnly) return MP_CONFIG_WRITABLE;
    return s_lastVersion > 1 ? MP_CONFIG_NEWER : MP_CONFIG_UNREADABLE;
}

bool mp_config_save(void) {
    Init();
    MpLock l(CfgMu());
    if (!mp_sys_mkdirs(mp_config_dir())) {
        mp_log("config: folder %s could not be made", mp_config_dir());
        return false;
    }
    char path[300], tmp[300], bak[300];
    Path(path, sizeof path, "");
    Path(tmp, sizeof tmp, ".tmp");
    Path(bak, sizeof bak, ".bak");
    if (s_readOnly) return false;   // keepsafe K1/K2/K4 (logged at load)
    // netfix B8b: the text is built in memory and written with POSIX calls (this runs on the helper thread too)
    std::string text;
    const auto addLine = [&text](const char* k, const char* v) {
        text += k;
        text += " = ";
        text += v;
        text += "\n";
    };
    const MpConfig& c = s_cfg;
    text += "# Super Mario Galaxy Vita: multiplayer settings (written by the game)\n";
    // savefix: a value the player did not change in this run is written as the file had it; keys this build does not know stay
    bool replaced[kRawMax] = {};
    Format(c, [&](const char* k, const char* v) {
        const RawLine* raw = FindLine(s_raw, s_rawCount, k);
        const RawLine* loaded = FindLine(s_loadedFmt, s_loadedCount, k);
        const bool keep = raw && loaded && !strcmp(loaded->val, v);
        if (raw && !keep) replaced[raw - s_raw] = true;
        addLine(k, keep ? raw->val : v);
    });
    for (int i = 0; i < s_rawCount; i++) {
        bool known = false;
        Format(c, [&](const char* k, const char*) { known = known || !strcmp(k, s_raw[i].key); });
        static const char* const kOptional[] = {"server", "upnp_permanent", "console_key"};   // (written only when set)
        for (const char* o : kOptional) known = known || !strcmp(o, s_raw[i].key);
        if (!known) addLine(s_raw[i].key, s_raw[i].val);
    }
    const bool written = mp_io_write_sync(tmp, text.data(), text.size());   // (savebug: synced before the rename)
    if (!written) mp_log("config: %s could not be written", tmp);
    const bool closed = true;
    MpConfig verify;
    Defaults(&verify);
    const bool parsed = written && closed && ParseFile(tmp, &verify);
    Clamp(&verify);
    if (!parsed || strcmp(verify.name, c.name) != 0 || verify.look != c.look ||
        memcmp(verify.consoleKey, c.consoleKey, 16) != 0 || verify.stun != (c.stun ? 1 : 0) ||
        verify.upnpPermanent != c.upnpPermanent) {
        remove(tmp);
        mp_log("config: write of %s failed", tmp);
        return false;
    }
    // (the Vita's rename does not replace an existing file: keep the previous one as .bak, then move the new one in)
    // savefix: only when mp.cfg is there: after a cut between the two renames mp.cfg.bak is the only copy, never removed here
    const bool hadPrimary = Exists(path);
    if (hadPrimary) {
        remove(bak);
        rename(path, bak);
    }
    if (rename(tmp, path) != 0) {
        if (hadPrimary) rename(bak, path);
        remove(tmp);
        mp_log("config: %s could not be replaced", path);
        return false;
    }
    // a value this run wrote over the file's own text is the file's text from now on (a later change back is the player's)
    int kept = 0;
    for (int i = 0; i < s_rawCount; i++)
        if (!replaced[i]) s_raw[kept++] = s_raw[i];
    s_rawCount = kept;
    s_entryDirty = false;   // (whatever was waiting is in the file now)
    mp_log("config: saved %s", path);
    return true;
}

bool mp_config_console_key(uint8_t out[16]) {
    Init();
    if (!KeySet(s_cfg.consoleKey)) {
        uint8_t fresh[16];
        if (!mp_sys_random_fill(fresh, 16)) {
            // smallfix: no secure random source: no key is made or kept (a weak key written to mp.cfg would stay for good); `out` is zeroed
            // and the caller refuses the session (MpCore::randomOk). The next call tries again.
            memset(out, 0, 16);
            mp_log("config: console key NOT made: the secure random source failed");
            return false;
        }
        memcpy(s_cfg.consoleKey, fresh, 16);
        s_cfg.consoleKey[0] |= 1;   // (never all zero)
        // (kept for this run even when the file cannot be written: then a restart makes a new one; keepsafe: a read-only mp.cfg is
        // never written over, mp_config_save refuses)
        mp_log("config: console key made%s", mp_config_save() ? "" : " (not saved)");
    }
    memcpy(out, s_cfg.consoleKey, 16);
    return true;
}

MpSettings mp_config_host_settings(MpMode mode) {
    Init();
    MpSettings s;
    memset(&s, 0, sizeof s);
    s.mode = (uint8_t)(mode == MP_MODE_RACE ? MP_MODE_RACE : MP_MODE_TOGETHER);
    s.maxPlayers = s_cfg.maxPlayers;
    s.interaction = s_cfg.interaction;
    s.sharedKills = s_cfg.sharedKills;
    s.rivals = s_cfg.rivals;
    s.raceWaitSec = s_cfg.raceWaitSec;
    s.nameTags = s_cfg.nameTags;
    s.flags = (uint8_t)((s_cfg.dropIn ? 1 : 0) | 2 | 4);
    s.downed = s_cfg.downed;
    return s;
}

// entry screen (review2 #11): the remembered Single Player / Multiplayer choice. Multiplayer is written at once (the multiplayer menu opens next
// anyway). Single Player is only remembered in RAM here: its way on is the single-player file select, and nothing of ours may touch the SD card or
// fail there. The file is written later, at a harmless moment (mp_entry_flush: the file select list is up), or with any other write of mp.cfg.
// A failed write is only logged (mp_config_save): no error reaches the caller, and a failed flush is not retried (the RAM value is right).
extern "C" void mp_entry_remember(uint8_t choice, bool persistNow) {
    MpCfgHostAllocScope scope;
    if (choice > 1) return;
    if (mp_config()->entry != choice) {
        MpConfig c = *mp_config();
        c.entry = choice;
        mp_config_set(&c);
        s_entryDirty = true;
    }
    if (persistNow && s_entryDirty) {
        s_entryDirty = false;
        mp_config_save();
    }
}

extern "C" void mp_entry_flush(void) {
    if (!s_entryDirty) return;   // (the usual case: one bool test on the single-player path)
    MpCfgHostAllocScope scope;
    s_entryDirty = false;
    mp_config_save();
}
