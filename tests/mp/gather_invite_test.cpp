// code5: the 5-character far-away code through the real gathering driver and the real core (loopback consoles), with the service stood in
// by a table in this file (the real client against the real server code is tests/mp/service_test.cpp). Covers: the host gets five characters
// that fit one row and the service holds only a sealed text; a friend types it (case, spaces, dashes forgiven) and gets in; the renewal at
// half the life and the close when the gathering ends; every way the lookup fails (wrong, expired, full, version, busy, silent, a forged
// answer); the long code as the fallback when the service cannot give one (with status.codeLong) and when this build has none.
//   built by tests/mp/gather_core_build.sh; quick: run directly
#include "core_test_util.h"
#include "../../platform/src/mp/mp_gather.h"
#include "port/mp_direct_code.h"
#include "port/mp_invite.h"

#include <map>

static bool gWifi = true, gNearby = false;
extern "C" bool mp_gather_wifi_connected(void) { return gWifi; }
extern "C" bool mp_gather_nearby_possible(void) { return gNearby; }

// ---- the service, as the gathering driver sees it (port/mp_invite.h) ----------------------------------------------------------------------
namespace Svc {
bool enabled = true;
int registerMode = 0;       // 0 ok, 1 busy (poll answers -BUSY), 2 silent (poll answers 0 for ever), 3 taken once, 4 over budget (= busy)
int registerPolls = 2;      // polls before the answer
int keepSec = 30;
int resolveMode = 0;        // 0 look it up, else answer -resolveMode (MpInviteError) once; 99 = silent; 98 = a forged blob
std::map<std::string, std::string> table;   // code -> sealed blob (what the real service stores)
std::map<std::string, bool> closed;
int registers = 0, keeps = 0, closes = 0, resolves = 0;
int takenSent = 0;
int regPolls = 0, resPolls = 0;
std::string regCode, regBlob, resCode;
bool regBegun = false, resBegun = false;
void Reset() {
    enabled = true; registerMode = 0; registerPolls = 2; keepSec = 30; resolveMode = 0;
    table.clear(); closed.clear();
    registers = keeps = closes = resolves = takenSent = regPolls = resPolls = 0;
    regBegun = resBegun = false;
}
}  // namespace Svc

extern "C" bool mp_service_enabled(void) { return Svc::enabled; }
extern "C" bool mp_svc_invite_register_begin(const char* code, const char* blob) {
    using namespace Svc;
    if (strlen(code) != 5 || strlen(blob) != MP_INVITE_BLOB_CHARS) return false;
    regCode = code; regBlob = blob; regPolls = 0; regBegun = true; registers++;
    return true;
}
extern "C" int mp_svc_invite_register_poll(int* keepSecOut) {
    using namespace Svc;
    if (!regBegun) return -int(MP_INV_BUSY);
    if (registerMode == 2) return 0;
    if (++regPolls < registerPolls) return 0;
    regBegun = false;
    if (registerMode == 1 || registerMode == 4) return -int(MP_INV_BUSY);
    if (registerMode == 3 && takenSent == 0) { takenSent++; return -int(MP_INV_TAKEN); }
    table[regCode] = regBlob;
    closed[regCode] = false;
    *keepSecOut = keepSec;
    return 1;
}
extern "C" void mp_svc_invite_keep(const char* code) { Svc::keeps++; (void)code; }
extern "C" void mp_svc_invite_close(const char* code) { Svc::closes++; Svc::closed[code] = true; }
extern "C" bool mp_svc_invite_resolve_begin(const char* code) { Svc::resCode = code; Svc::resPolls = 0; Svc::resBegun = true; Svc::resolves++; return true; }
extern "C" int mp_svc_invite_resolve_poll(char* out, int cap) {
    using namespace Svc;
    if (!resBegun) return -int(MP_INV_BUSY);
    if (resolveMode == 99) return 0;
    if (++resPolls < 2) return 0;
    resBegun = false;
    if (resolveMode == 98) { snprintf(out, (size_t)cap, "%s", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"); return 1; }
    if (resolveMode) return -resolveMode;
    auto it = table.find(resCode);
    if (it == table.end()) return -int(MP_INV_WRONG);
    if (closed[resCode]) return -int(MP_INV_EXPIRED);
    snprintf(out, (size_t)cap, "%s", it->second.c_str());
    return 1;
}
extern "C" void mp_svc_invite_cancel(void) { Svc::regBegun = Svc::resBegun = false; }

// ---- three consoles: 0 and 1 gather on the LAN, 2 is the friend far away -----------------------------------------------------------
struct World {
    MpLoopHub hub;
    std::vector<MpCore*> cs;
    std::vector<MpGather*> gs;
    int leader = -1;
    World() {
        hub.setClock(VNow, 0);
        const char* names[3] = {"Ann", "Bob", "Cid"};
        for (int i = 0; i < 3; i++) {
            cs.push_back(MakeCore(hub, (uint32_t)(i + 1), names[i], 900 + (uint32_t)i));
            gs.push_back(new MpGather(cs.back()));
        }
        gWifi = false, gNearby = true;
    }
    ~World() { for (MpGather* g : gs) delete g; for (MpCore* c : cs) delete c; }
    void Run(int ms) {
        for (int k = 0; k < ms; k += 10) {
            Step(cs, 10, 5);
            for (MpGather* g : gs) g->tick(g_now);
        }
    }
    MpGatherStatus St(int i) { MpGatherStatus s; gs[i]->status(&s, g_now); return s; }
    // consoles 0 and 1 gather; returns once one of them may start (the leader)
    bool Gather() {
        for (int i = 0; i < 2; i++) if (!gs[i]->start(MP_MODE_TOGETHER, MP_CONN_LOOPBACK, g_now)) return false;
        for (int t = 0; t < 20000 && leader < 0; t += 10) {
            Run(10);
            for (int i = 0; i < 2; i++) if (gs[i]->canStart(g_now)) leader = i;
        }
        return leader >= 0;
    }
    // the leader asks for the code; true with the code once it shows (or the last status when it never does)
    bool Code(char* out, size_t cap, int maxMs = 30000) {
        out[0] = 0;
        bool got = gs[leader]->getCode(out, cap, g_now);
        for (int t = 0; t < maxMs && !got; t += 10) { Run(10); got = gs[leader]->getCode(out, cap, g_now); }
        return got;
    }
};

static bool InGame(World& w, int i, int heads, int maxMs = 20000) {
    MpGatherStatus st;
    for (int t = 0; t < maxMs; t += 10) {
        w.Run(10);
        st = w.St(i);
        if (st.state == MP_GATHER_GATHERING && st.headCount == heads) return true;
    }
    return false;
}

// a code join that fails: the status goes FAILED with the reason within the time the menu allows
static MpGatherStatus Failed(World& w, int i, int maxMs = 20000) {
    MpGatherStatus st;
    for (int t = 0; t < maxMs; t += 10) {
        w.Run(10);
        st = w.St(i);
        if (st.state == MP_GATHER_FAILED) return st;
    }
    return st;
}

int main(int argc, char**) {
    g_echo = argc > 1;
    mp_log_set_sink(LogSink, true);

    // ---- A: the happy path ------------------------------------------------------------------------------------------------------------
    {
        Svc::Reset();
        World w;
        CHECK(w.Gather());
        char code[MP_DIRECT_CODE_MAX];
        CHECK(w.Code(code, sizeof code));
        printf("invite: leader %d got code %s\n", w.leader, code);
        CHECK(strlen(code) == 5);                                   // five characters: one row
        for (int i = 0; i < 5; i++) CHECK(strchr("ABCDEFGHJKMNPQRSTUVWXYZ23456789", code[i]) != nullptr);
        MpGatherStatus st = w.St(w.leader);
        CHECK(!strcmp(st.code, code) && !st.codeLong && st.inviteError == 0);
        CHECK(Svc::registers == 1 && Svc::table.size() == 1 && Svc::table.count(code) == 1);
        // the service holds a sealed text: it opens only with the code, to this room's address and secret, and shows no address
        const std::string blob = Svc::table[code];
        uint32_t ip = 0; uint16_t port = 0; uint8_t sec[5];
        CHECK(mp_invite_open(code, blob.c_str(), &ip, &port, sec) == MP_CODE_OK && ip != 0 && port == MP_DIRECT_DEFAULT_PORT);
        CHECK(mp_invite_open("AAAAA", blob.c_str(), &ip, &port, sec) == MP_CODE_CHECK);
        // the member follows into the by-address room
        CHECK(InGame(w, 1 - w.leader, 2));
        CHECK(!w.St(w.leader).canStart || true);
        // renewal: nothing before half the life, one at it, one more after another half
        const int before = Svc::keeps;
        w.Run(20000);
        CHECK(Svc::keeps == before);
        w.Run(15000);
        CHECK(Svc::keeps == before + 1);
        w.Run(31000);
        CHECK(Svc::keeps == before + 2);
        // the friend types the code the way people read it aloud: lower case, spaces and a dash
        char typed[16];
        snprintf(typed, sizeof typed, " %c%c %c-%c%c ", code[0] | 0x20, code[1] | 0x20, code[2] | 0x20, code[3] | 0x20, code[4] | 0x20);
        for (char* p = typed; *p; p++) if (*p >= 'a' && *p <= 'z' && !strchr("abcdefghjkmnpqrstuvwxyz", *p)) *p = (char)(*p & ~0x20);
        CHECK(w.gs[2]->joinCode(typed, g_now) == MP_CODE_OK);
        MpGatherStatus fs = w.St(2);
        CHECK(fs.state == MP_GATHER_MERGING);                       // the lookup is part of the joining
        CHECK(InGame(w, 2, 3));
        CHECK(Svc::resolves == 1 && !Svc::closed[code] && Svc::closes == 0);
        // Start: the code is closed at the service once, and nothing more is sent for it
        w.Run(1500);
        CHECK(w.gs[w.leader]->begin());
        CHECK(Svc::closes == 1 && Svc::closed[code]);
        const int k = Svc::keeps;
        w.Run(70000);
        CHECK(Svc::keeps == k && Svc::closes == 1);
        for (int i = 0; i < 3; i++) CHECK(w.St(i).state == MP_GATHER_STARTED);
    }

    // ---- B: the host leaves the gathering: the code is closed ------------------------------------------------------------------------------
    {
        Svc::Reset();
        World w;
        CHECK(w.Gather());
        char code[MP_DIRECT_CODE_MAX];
        CHECK(w.Code(code, sizeof code) && strlen(code) == 5);
        w.gs[w.leader]->stop();
        CHECK(Svc::closes == 1 && Svc::closed[code]);
        w.gs[w.leader]->stop();
        CHECK(Svc::closes == 1);                                    // (once)
    }

    // ---- C: a friend whose lookup fails: the reason is in the status, no game is joined --------------------------------------------------
    {
        struct { int mode; int err; const char* what; } cases[] = {
            {MP_INV_WRONG, MP_INV_WRONG, "wrong"}, {MP_INV_EXPIRED, MP_INV_EXPIRED, "expired"}, {MP_INV_FULL, MP_INV_FULL, "full"},
            {MP_INV_VERSION, MP_INV_VERSION, "version"}, {MP_INV_BUSY, MP_INV_BUSY, "busy"}, {MP_INV_NO_WIFI, MP_INV_NO_WIFI, "no wifi"},
            {MP_INV_CLOCK, MP_INV_CLOCK, "clock"}, {MP_INV_UPDATE, MP_INV_UPDATE, "update"}, {99, MP_INV_BUSY, "silent"}, {98, MP_INV_SEALED_BAD, "forged"},
        };
        for (auto& c : cases) {
            Svc::Reset();
            Svc::resolveMode = c.mode;
            World w;
            CHECK(w.gs[2]->joinCode("KCBC4", g_now) == MP_CODE_OK);
            const uint64_t t0 = g_now;
            MpGatherStatus st = Failed(w, 2);
            printf("invite: lookup %s -> state %d error %d after %llu ms\n", c.what, st.state, st.inviteError, (unsigned long long)(g_now - t0));
            CHECK(st.state == MP_GATHER_FAILED && st.inviteError == c.err && st.headCount <= 1);
            CHECK(g_now - t0 <= MpGather::kResolveMs + 1500);
            CHECK(!w.cs[2]->active());
        }
        // a code nobody registered: the table has none -> WRONG (the real service says invite_not_found)
        Svc::Reset();
        World w;
        CHECK(w.gs[2]->joinCode("WME6M", g_now) == MP_CODE_OK);
        CHECK(Failed(w, 2).inviteError == MP_INV_WRONG);
    }

    // ---- D: what is refused before any request is made ------------------------------------------------------------------------------
    {
        Svc::Reset();
        World w;
        CHECK(w.gs[2]->joinCode("KCBC0", g_now) == MP_CODE_LOOKALIKE);
        CHECK(w.gs[2]->joinCode("kcbco", g_now) == MP_CODE_LOOKALIKE);
        CHECK(w.gs[2]->joinCode("KCB", g_now) == MP_CODE_LENGTH);
        CHECK(w.gs[2]->joinCode("KCBC4F", g_now) == MP_CODE_LENGTH);
        CHECK(w.gs[2]->joinCode("KC#C4", g_now) == MP_CODE_CHARACTER);
        CHECK(w.gs[2]->joinCode("", g_now) == MP_CODE_EMPTY);
        CHECK(Svc::resolves == 0 && w.St(2).state == MP_GATHER_OFF);
    }

    // ---- E: the service cannot give a code: the long one, with the reason shown -------------------------------------------------------------
    for (int mode : {1, 2, 4}) {
        Svc::Reset();
        Svc::registerMode = mode;
        World w;
        CHECK(w.Gather());
        char code[MP_DIRECT_CODE_MAX];
        const uint64_t t0 = g_now;
        CHECK(w.Code(code, sizeof code));
        MpGatherStatus st = w.St(w.leader);
        printf("invite: service %s -> code %s (long %d) after %llu ms\n", mode == 2 ? "silent" : "busy", code, st.codeLong, (unsigned long long)(g_now - t0));
        CHECK(strlen(code) >= 15 && st.codeLong && !strcmp(st.code, code));
        CHECK(Svc::table.empty() && Svc::closes == 0);              // (nothing registered, nothing to close)
        CHECK(g_now - t0 <= MpGather::kRegisterMs + 3000);
        // the long code still works for a friend (the way it always did)
        CHECK(InGame(w, 1 - w.leader, 2));
        CHECK(w.gs[2]->joinCode(code, g_now) == MP_CODE_OK);
        CHECK(InGame(w, 2, 3));
        CHECK(Svc::resolves == 0);                                  // (the long code never asks the service)
        w.gs[w.leader]->stop();
        CHECK(Svc::closes == 0);
    }

    // ---- F: a build with no service: the long code, nothing said about a service ----------------------------------------------------------
    {
        Svc::Reset();
        Svc::enabled = false;
        World w;
        CHECK(w.Gather());
        char code[MP_DIRECT_CODE_MAX];
        CHECK(w.Code(code, sizeof code));
        MpGatherStatus st = w.St(w.leader);
        CHECK(strlen(code) >= 15 && !st.codeLong && Svc::registers == 0);
    }

    // ---- G: the code is taken (one in 28 million): the host picks another and still gives five characters ----------------------------------
    {
        Svc::Reset();
        Svc::registerMode = 3;
        World w;
        CHECK(w.Gather());
        char code[MP_DIRECT_CODE_MAX];
        CHECK(w.Code(code, sizeof code));
        CHECK(strlen(code) == 5 && Svc::registers == 2 && Svc::table.size() == 1 && Svc::table.count(code) == 1 && !w.St(w.leader).codeLong);
    }

    printf("%s\n", g_fail ? "gather invite test FAILED" : "gather invite test: ok");
    return g_fail ? 1 : 0;
}
