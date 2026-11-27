// flow (keepsafe F1-F4): flow.cfg / coop.cfg across game versions. A newer game's file is read for the keys this build knows and never
// rewritten; a damaged file is copied aside (.unreadable) before it is written over; unknown lines (keys, co-op file numbers) survive a
// write; a line of any length is read whole; Played With keeps 20 names, newest first, with their codes.
//   bash tests/mp/flow_store_build.sh
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <string>

#include "port/mp_bridge.h"
#include "port/mp_ui_flow.h"

extern "C" bool mp_gather_wifi_connected(void);
extern "C" bool mp_gather_nearby_possible(void);
static int gChecks, gFails;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string gDir;
// ---- what mp_ui_flow.cpp needs from the rest of the port (small fakes)
const char* mp_config_dir(void) { return gDir.c_str(); }
extern "C" bool mp_core_resume(void) { return false; }
// wire: the core's answer for a player's friend code (core2); the test sets what it announces
static const char* gSlotCode[4] = {"ABCD-1234", nullptr, nullptr, nullptr};
extern "C" bool mp_core_slot_friend_code(int slot, char* out, size_t cap) {
    if (out && cap) out[0] = 0;
    if (slot < 0 || slot >= 4 || !gSlotCode[slot] || cap < 10) return false;
    snprintf(out, cap, "%s", gSlotCode[slot]);
    return true;
}
extern "C" {
void mp_log(const char* fmt, ...) { va_list a; va_start(a, fmt); if (getenv("FLOW_LOG")) { vfprintf(stderr, fmt, a); fputc('\n', stderr); } va_end(a); }
void mp_ui_profile(MpProfile* p) { memset(p, 0, sizeof *p); }
}
bool mp_sys_mkdirs(const char* d) { std::string c = "mkdir -p '" + std::string(d) + "'"; return system(c.c_str()) == 0; }
void mp_name_clean(const char* in, size_t len, char* out) { size_t n = len < MP_NAME_BYTES ? len : MP_NAME_BYTES; memcpy(out, in, n); out[n] = 0; }
void mp_name_or_character(char* name, int look) { if (!name[0]) snprintf(name, MP_NAME_BYTES + 1, "Char%d", look); }

static std::string Read(const char* name) {
    std::string p = gDir + "/" + name, o;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return "<none>";
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) o.append(b, n);
    fclose(f);
    return o;
}
static void Write(const char* name, const std::string& t) {
    std::string p = gDir + "/" + name;
    FILE* f = fopen(p.c_str(), "wb");
    fwrite(t.data(), 1, t.size(), f);
    fclose(f);
}
static void Fresh(const char* tag) {
    // (a folder of this run's own: a fixed /tmp/flow_store_test was shared by every tree running the test at the same time)
    static std::string root;
    if (root.empty()) {
        char t[] = "/tmp/flow_store_testXXXXXX";
        if (!mkdtemp(t)) abort();
        root = t;
    }
    gDir = root + "/" + tag;
    std::string c = "rm -rf '" + gDir + "' && mkdir -p '" + gDir + "'";
    if (system(c.c_str()) != 0) abort();
    mp_ui_flow_forget();
}
static bool Has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

int main() {
    // wire (a): the menu's hook is the core's answer, not a constant "no code" (it was a stub); (c) the gathering driver's radio questions
    // are defined by the port (nothing defined them: the driver never offered Nearby)
    {
        char c[10] = "xxxxxxxxx";
        CHECK(mp_ui_slot_friend_code && mp_ui_slot_friend_code(0, c) && !strcmp(c, "ABCD-1234"));
        CHECK(mp_ui_slot_friend_code(1, c) == false && c[0] == 0);
        CHECK(mp_ui_slot_friend_code(9, c) == false && c[0] == 0);
        setenv("SMG_MP_TEST_NO_WIFI", "0", 1);
        CHECK(mp_gather_wifi_connected());
        setenv("SMG_MP_TEST_NO_WIFI", "1", 1);
        CHECK(!mp_gather_wifi_connected());
        unsetenv("SMG_MP_TEST_NO_WIFI");
        unsetenv("SMG_MP_TEST_NEARBY");
        CHECK(!mp_gather_nearby_possible());
        setenv("SMG_MP_TEST_NEARBY", "1", 1);
        CHECK(mp_gather_nearby_possible());
        unsetenv("SMG_MP_TEST_NEARBY");
    }
    // a fresh write: the version line first, Played With with codes, newest first, up to 20
    Fresh("fresh");
    MpFlowStore fs;
    memset(&fs, 0, sizeof fs);
    CHECK(!mp_flow_store_get(&fs));
    fs.charPicked = 1, fs.lastCoop = 2, fs.fallsAsked = 1;
    fs.playedCount = MP_PLAYED_WITH;
    for (int i = 0; i < MP_PLAYED_WITH; i++) {
        snprintf(fs.playedWith[i], sizeof fs.playedWith[i], "P%d", i);
        if (i % 2 == 0) snprintf(fs.playedCode[i], sizeof fs.playedCode[i], "AB%02d-CD%02d", i, i);
    }
    mp_flow_store_set(&fs);
    std::string t = Read("flow.cfg");
    CHECK(t.rfind("version = 1\n", 0) == 0 && Has(t, "played = P0\nplayed_code = AB00-CD00\nplayed = P1\nplayed = P2\n"));
    mp_ui_flow_forget();
    MpFlowStore back;
    memset(&back, 0, sizeof back);
    CHECK(mp_flow_store_get(&back) && back.playedCount == 20 && !strcmp(back.playedWith[19], "P19") && !strcmp(back.playedCode[18], "AB18-CD18") &&
          back.playedCode[19][0] == 0 && back.lastCoop == 2 && back.fallsAsked == 1);
    // F1: a newer game's file: read for the keys known, never rewritten
    Fresh("newer");
    const std::string newer = "version = 2\nchar_picked = 1\nlast_coop = 3\nnew_thing = 7\n";
    Write("flow.cfg", newer);
    memset(&back, 0, sizeof back);
    CHECK(mp_flow_store_get(&back) && back.lastCoop == 3);
    back.lastCoop = 4;
    mp_flow_store_set(&back);
    CHECK(Read("flow.cfg") == newer);
    memset(&back, 0, sizeof back);
    CHECK(mp_flow_store_get(&back) && back.lastCoop == 4);   // (the change stays in memory this run)
    // F3: an older-format file (no version) with a newer build's key: the key survives a write
    Fresh("unknown");
    Write("flow.cfg", "char_picked = 1\nsomething_new = hello world\nlast_coop = 1\n");
    memset(&back, 0, sizeof back);
    mp_flow_store_get(&back);
    back.lastCoop = 5;
    mp_flow_store_set(&back);
    t = Read("flow.cfg");
    CHECK(t.rfind("version = 1\n", 0) == 0 && Has(t, "last_coop = 5") && Has(t, "something_new = hello world\n"));
    // F2: a damaged file is copied aside before it is written over (and the copy is never replaced)
    Fresh("damaged");
    const std::string bad = "char_picked = 1\n\x01\x02 garbage without an equals sign\nlast_coop = 2\n";
    Write("flow.cfg", bad);
    memset(&back, 0, sizeof back);
    CHECK(mp_flow_store_get(&back) && back.lastCoop == 2);
    back.lastCoop = 6;
    mp_flow_store_set(&back);
    CHECK(Read("flow.cfg.unreadable") == bad && Has(Read("flow.cfg"), "last_coop = 6"));
    mp_flow_store_set(&back);
    CHECK(Read("flow.cfg.unreadable") == bad);
    // F4: a very long line is read whole (its tail is never taken for a key)
    Fresh("long");
    Write("flow.cfg", "char_picked = 1\nlast_coop = 1\nlong_key = " + std::string(300, 'x') + "last_coop = 5\n");
    memset(&back, 0, sizeof back);
    CHECK(mp_flow_store_get(&back) && back.lastCoop == 1);
    // coop.cfg: a co-op file number this build does not know, and an unknown key in a known file: both kept where they were
    Fresh("coop");
    Write("coop.cfg", "file = 2\ngalaxy = 5\nplayer = 0 Alex\nshine = 3\nfile = 7\ngalaxy = 9\nplayer = 1 Ros\n");
    MpCoopMeta m;
    memset(&m, 0, sizeof m);
    CHECK(mp_coop_meta_get(2, &m) && m.count == 1 && m.galaxy == 5);
    m.galaxy = 6;
    mp_coop_meta_set(2, &m);
    t = Read("coop.cfg");
    CHECK(t.rfind("version = 1\n", 0) == 0 && Has(t, "file = 2\ngalaxy = 6\nplayer = 0 Alex\n") && Has(t, "file = 2\nshine = 3\n") &&
          Has(t, "file = 7\ngalaxy = 9\nplayer = 1 Ros\n"));
    mp_ui_flow_forget();
    memset(&m, 0, sizeof m);
    CHECK(mp_coop_meta_get(2, &m) && m.galaxy == 6 && m.count == 1);   // (read again: the same)
    // coop.cfg of a newer game: never rewritten
    Fresh("coopnewer");
    Write("coop.cfg", "version = 3\nfile = 1\ngalaxy = 2\n");
    memset(&m, 0, sizeof m);
    mp_coop_meta_set(1, &m);
    CHECK(Read("coop.cfg") == "version = 3\nfile = 1\ngalaxy = 2\n");
    fprintf(stderr, "%d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
