// Galaxy 3.0 multiplayer (beta diagnostics): the session summary, the "something went wrong" memory, and "Send a Problem Report". See port/mp_diag.h.
// The connection check is in mp_netcheck.cpp (it stores its result here: mp_diag_store_check).
extern "C" void port_host_alloc_scope_enter(void) __attribute__((weak));
extern "C" void port_host_alloc_scope_leave(void) __attribute__((weak));
namespace {
struct DiagHostAllocScope {
    DiagHostAllocScope() { if (port_host_alloc_scope_enter) port_host_alloc_scope_enter(); }
    ~DiagHostAllocScope() { if (port_host_alloc_scope_leave) port_host_alloc_scope_leave(); }
};
}  // namespace
#define DIAG_HOST_ALLOC DiagHostAllocScope diagHostAllocScope_

#define MP_DIAG_DEFINES_STRONG 1   // (this unit defines the mp_diag_* functions: strong, see port/mp_diag.h)
#include "port/mp_diag.h"

#include "mp_core_sys.h"
#include "mp_diag_int.h"
#include "mp_posix_io.h"
#include "mp_report.h"
#include "mp_service.h"
#include "port/mp_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern "C" void NpNetSetLeaveSummary(const char* json) __attribute__((weak));   // net/src/vlink_online.cpp
extern "C" const char* port_log_current_path(void) __attribute__((weak));       // platform/src/os/os_alarm.cpp (desktop): where game.log is

namespace {

// ---- where things live -----------------------------------------------------------------------------------------------------------------
void DataDir(char* buf, size_t cap) {
#ifdef __vita__
    snprintf(buf, cap, "ux0:data/smg");
#else
    const char* d = getenv("PETARI_DATA_DIR");
    snprintf(buf, cap, "%s", d && *d ? d : ".");
#endif
}

void StatePath(char* buf, size_t cap) {
    char d[200];
    DataDir(d, sizeof d);
    snprintf(buf, cap, "%s/mp/diag_last.txt", d);
}

// ---- state ---------------------------------------------------------------------------------------------------------------------------------
struct Sess {
    bool on;
    int kind;                 // 0 Nearby / Same Wi-Fi / loopback, 1 online / by address
    uint64_t startMs;
    uint32_t rx, txFail, members;
    bool relay;
    MpDiagPose pose;
};
Sess g_s;
MpDiagSummary g_last;         // the last session's numbers
MpDiagCheck g_check;          // the last finished check
bool g_errPending = false;
char g_errCode[40];
bool g_loaded = false;

// A session's numbers into a summary. `ended`: the end reason text; durS from the clock.
void Compose(MpDiagSummary* o, const char* ended, uint64_t nowMs) {
    memset(o, 0, sizeof *o);
    o->valid = true;
    snprintf(o->transport, sizeof o->transport, "%s", g_s.relay ? "relay" : "udp");
    const uint64_t ms = nowMs > g_s.startMs ? nowMs - g_s.startMs : 0;
    o->durS = static_cast<uint32_t>(ms / 1000);
    o->members = g_s.members;
    snprintf(o->ended, sizeof o->ended, "%s", ended);
    o->rxRate = ms >= 1000 ? static_cast<float>(g_s.rx) * 1000.0f / static_cast<float>(ms) : 0.0f;
    const MpDiagPose& p = g_s.pose;
    o->gapMeanMs = p.gaps ? p.gapSumMs / static_cast<float>(p.gaps) : 0.0f;
    o->gapMaxMs = p.gapMaxMs;
    o->bufMeanMs = p.samples ? p.depthSumMs / static_cast<float>(p.samples) : 0.0f;
    o->bufMaxMs = p.depthMaxMs;
    o->extrap = p.extrap;
    o->hold = p.hold;
    o->snap = p.snap;
    o->late = p.late;
    o->stall100 = p.stall100;
    o->txFail = g_s.txFail;
    o->relayUsed = g_s.relay;
    snprintf(o->app, sizeof o->app, "%s", mp_service_app_version());
}

// "leave" | "host_lost" | "kicked" | "error:<code>"
void EndedText(int err, const char* why, char* out, size_t cap) {
    (void)why;
    switch (err) {
    case MP_ERR_NONE: snprintf(out, cap, "leave"); break;
    case MP_ERR_HOST_LEFT: snprintf(out, cap, "host_lost"); break;
    case MP_ERR_KICKED: snprintf(out, cap, "kicked"); break;
    case MP_ERR_NO_WIFI: snprintf(out, cap, "error:no_wifi"); break;
    case MP_ERR_SERVICE_DOWN: snprintf(out, cap, "error:service_down"); break;
    case MP_ERR_UPDATE_REQUIRED: snprintf(out, cap, "error:update_required"); break;
    case MP_ERR_ROOM_NOT_FOUND: snprintf(out, cap, "error:room_not_found"); break;
    case MP_ERR_ROOM_FULL: snprintf(out, cap, "error:room_full"); break;
    case MP_ERR_DIFFERENT_VERSION: snprintf(out, cap, "error:different_version"); break;
    case MP_ERR_TIMEOUT: snprintf(out, cap, "error:timeout"); break;
    case MP_ERR_NAT: snprintf(out, cap, "error:nat"); break;
    case MP_ERR_NAME_TAKEN: snprintf(out, cap, "error:name_taken"); break;
    case MP_ERR_BAD_NAME: snprintf(out, cap, "error:bad_name"); break;
    case MP_ERR_RATE_LIMITED: snprintf(out, cap, "error:rate_limited"); break;
    case MP_ERR_CLOCK: snprintf(out, cap, "error:clock"); break;
    default: snprintf(out, cap, "error:internal"); break;
    }
}

// ---- the small state file (the last numbers survive a restart: a crash is the usual reason for a report) -------------------------------------------
void Save() {
    char path[260];
    StatePath(path, sizeof path);
    char dir[240];
    snprintf(dir, sizeof dir, "%s", path);
    char* slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0777); }
    char t[1024];
    int n = snprintf(t, sizeof t, "E %d %s\n", g_errPending ? 1 : 0, g_errCode[0] ? g_errCode : "-");
    if (g_last.valid && n > 0 && n < static_cast<int>(sizeof t) - 300)
        n += snprintf(t + n, sizeof t - n, "S %s %u %u %s %.2f %.2f %.2f %.2f %.2f %u %u %u %u %u %u %d %s\n", g_last.transport, (unsigned)g_last.durS, (unsigned)g_last.members,
                      g_last.ended, g_last.rxRate, g_last.gapMeanMs, g_last.gapMaxMs, g_last.bufMeanMs, g_last.bufMaxMs, (unsigned)g_last.extrap, (unsigned)g_last.hold,
                      (unsigned)g_last.snap, (unsigned)g_last.late, (unsigned)g_last.stall100, (unsigned)g_last.txFail, g_last.relayUsed ? 1 : 0, g_last.app);
    if (g_check.valid && n > 0 && n < static_cast<int>(sizeof t) - 150)
        n += snprintf(t + n, sizeof t - n, "C %s %d %s %d %s\n", g_check.nat, g_check.stunMs, g_check.relay, g_check.relayRttMs, g_check.err[0] ? g_check.err : "-");
    if (n <= 0) return;
    char tmp[270];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (mp_io_write(tmp, t, static_cast<size_t>(n))) {
        remove(path);
        if (rename(tmp, path) != 0) remove(tmp);
    }
}

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    char path[260];
    StatePath(path, sizeof path);
    char t[1100];
    const long n = mp_io_read(path, t, sizeof t - 1);
    if (n <= 0) return;
    t[n] = 0;
    for (char* line = t; line && *line;) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (line[0] == 'E' && line[1] == ' ') {
            int p = 0;
            char code[40] = "";
            if (sscanf(line + 2, "%d %39s", &p, code) >= 1) {
                g_errPending = p == 1;
                snprintf(g_errCode, sizeof g_errCode, "%s", strcmp(code, "-") ? code : "");
            }
        } else if (line[0] == 'S' && line[1] == ' ') {
            MpDiagSummary s;
            memset(&s, 0, sizeof s);
            unsigned dur = 0, mem = 0, ex = 0, ho = 0, sn = 0, la = 0, st = 0, tx = 0;
            int relay = 0;
            char tr[8] = "", ended[40] = "", app[16] = "";
            if (sscanf(line + 2, "%7s %u %u %39s %f %f %f %f %f %u %u %u %u %u %u %d %15s", tr, &dur, &mem, ended, &s.rxRate, &s.gapMeanMs, &s.gapMaxMs, &s.bufMeanMs,
                       &s.bufMaxMs, &ex, &ho, &sn, &la, &st, &tx, &relay, app) == 17) {
                s.valid = true;
                snprintf(s.transport, sizeof s.transport, "%s", tr);
                s.durS = dur; s.members = mem;
                snprintf(s.ended, sizeof s.ended, "%s", ended);
                s.extrap = ex; s.hold = ho; s.snap = sn; s.late = la; s.stall100 = st; s.txFail = tx; s.relayUsed = relay != 0;
                snprintf(s.app, sizeof s.app, "%s", app);
                g_last = s;
            }
        } else if (line[0] == 'C' && line[1] == ' ') {
            MpDiagCheck c;
            memset(&c, 0, sizeof c);
            char nat[12] = "", relay[8] = "", err[40] = "";
            if (sscanf(line + 2, "%11s %d %7s %d %39s", nat, &c.stunMs, relay, &c.relayRttMs, err) == 5) {
                c.valid = true;
                snprintf(c.nat, sizeof c.nat, "%s", nat);
                snprintf(c.relay, sizeof c.relay, "%s", relay);
                snprintf(c.err, sizeof c.err, "%s", strcmp(err, "-") ? err : "");
                g_check = c;
            }
        }
        line = nl ? nl + 1 : nullptr;
    }
}

// ---- the report ----------------------------------------------------------------------------------------------------------------------------------
struct Report {
    bool active;
    uint64_t startMs;
    uint32_t timeoutMs;
} g_rep;

constexpr uint32_t kReportTimeoutMs = 30000;
constexpr size_t kCurRead = 96 * 1024, kPrevRead = 48 * 1024;

// The last `maxBytes` of a file; *isTail true when it does not start at the file's start. null when it cannot be read.
char* ReadTail(const char* path, size_t maxBytes, size_t* n, bool* isTail) {
    *n = 0;
    *isTail = false;
    long size = -1;
    char* b = static_cast<char*>(malloc(maxBytes + 1));
    if (!b) return nullptr;
    long got = mp_io_read(path, b, 0, 0, &size);
    if (got < 0) { free(b); return nullptr; }
    long from = 0;
    if (size > static_cast<long>(maxBytes)) { from = size - static_cast<long>(maxBytes); *isTail = true; }
    got = size > 0 ? mp_io_read(path, b, maxBytes, from) : 0;
    if (got < 0) got = 0;
    b[got] = 0;
    *n = static_cast<size_t>(got);
    return b;
}

const char* KindName(int k) { return k == 1 ? "after_error" : "user"; }
const char* WhereName(int w) { return w == 1 ? "settings" : (w == 2 ? "post_session" : "mp_menu"); }

}  // namespace

// ---- internal (mp_netcheck.cpp) ------------------------------------------------------------------------------------------------------------------
void mp_diag_store_check(const MpDiagCheck* c) {
    Load();
    g_check = *c;
    g_check.valid = true;
    Save();
}

// ---- the session ---------------------------------------------------------------------------------------------------------------------------------
extern "C" void mp_diag_session_begin(int kind) {
    Load();
    memset(&g_s, 0, sizeof g_s);
    g_s.on = true;
    g_s.kind = kind;
    g_s.startMs = mp_sys_now_ms();
    g_s.members = 1;
}

extern "C" void mp_diag_session_count(uint32_t rx, uint32_t txFail, uint32_t members, int relay) {
    if (!g_s.on) return;
    g_s.rx = rx;
    g_s.txFail = txFail;
    if (members > g_s.members) g_s.members = members;
    if (relay) g_s.relay = true;
}

extern "C" void mp_diag_note_pose(const MpDiagPose* totals) {
    if (g_s.on && totals) g_s.pose = *totals;
}

extern "C" void mp_diag_session_end(int err, const char* why, int online) {
    DIAG_HOST_ALLOC;
    if (!g_s.on) return;
    Load();
    char ended[40];
    EndedText(err, why, ended, sizeof ended);
    MpDiagSummary s;
    Compose(&s, ended, mp_sys_now_ms());
    g_last = s;
    g_s.on = false;
    if (mp_diag_error_offers_report(err)) {   // (the summary's "ended" is recorded and sent in every case; only the offer is limited)
        g_errPending = true;
        snprintf(g_errCode, sizeof g_errCode, "%s", ended);
    }
    Save();
    char json[600];
    const size_t n = mp_diag_summary_json(&s, json, sizeof json);
    mp_log("diag: session summary %s", n ? json : "(none)");
    // an online room's leave request carries it (a Nearby / Same Wi-Fi session has no room at the service)
    if (online && n && NpNetSetLeaveSummary) NpNetSetLeaveSummary(json);
}

extern "C" bool mp_diag_last_summary(MpDiagSummary* out) {
    Load();
    if (!g_last.valid) return false;
    if (out) *out = g_last;
    return true;
}

extern "C" bool mp_diag_last_check(MpDiagCheck* out) {
    Load();
    if (!g_check.valid) return false;
    if (out) *out = g_check;
    return true;
}

// ---- the after-error offer ----------------------------------------------------------------------------------------------------------------------
extern "C" bool mp_diag_error_pending(void) { Load(); return g_errPending; }
extern "C" void mp_diag_error_clear(void) {
    Load();
    if (!g_errPending) return;
    g_errPending = false;
    Save();
}

// ---- the account / service ------------------------------------------------------------------------------------------------------------------------
extern "C" bool mp_diag_has_account(void) {
    char url[200], tok[96];
    return mp_service_credentials(url, sizeof url, tok, sizeof tok);
}
extern "C" bool mp_diag_service_enabled(void) { return mp_service_enabled(); }

// ---- Send a Problem Report ---------------------------------------------------------------------------------------------------------------------------
extern "C" bool mp_diag_report_begin(int kind, int where, const char* note) {
    DIAG_HOST_ALLOC;
    Load();
    mp_service_post_cancel();
    g_rep.active = false;
    if (!mp_diag_has_account()) return false;
    // the logs: this run's (the desktop's game.log through port_log_current_path) and the previous one's (<log>.prev)
    char logPath[300], prevPath[310], dd[200];
    DataDir(dd, sizeof dd);
    const char* cur = port_log_current_path ? port_log_current_path() : nullptr;
    if (cur && *cur) snprintf(logPath, sizeof logPath, "%s", cur);
    else snprintf(logPath, sizeof logPath, "%s/log.txt", dd);
    snprintf(prevPath, sizeof prevPath, "%s.prev", logPath);
    size_t curLen = 0, prevLen = 0;
    bool curTail = false, prevTail = false;
    char* curText = ReadTail(logPath, kCurRead, &curLen, &curTail);
    char* prevText = ReadTail(prevPath, kPrevRead, &prevLen, &prevTail);

    MpDiagCheck check;
    MpDiagSummary stats;
    const bool haveCheck = mp_diag_last_check(&check);
    const bool haveStats = mp_diag_last_summary(&stats);
    MpDiagBodyIn in;
    memset(&in, 0, sizeof in);
    in.kind = KindName(kind);
    in.where = WhereName(where);
    in.note = note;
    in.check = haveCheck ? &check : nullptr;
    in.stats = haveStats ? &stats : nullptr;
    in.logPrev = prevText; in.logPrevLen = prevLen; in.logPrevIsTail = prevTail;
    in.logCur = curText; in.logCurLen = curLen; in.logCurIsTail = curTail;
    const size_t cap = MP_DIAG_BODY_MAX + 1024;
    char* body = static_cast<char*>(malloc(cap));
    size_t logBytes = 0;
    const size_t n = body ? mp_diag_build_body(&in, body, cap, MP_DIAG_BODY_MAX, &logBytes) : 0;
    free(curText);
    free(prevText);
    bool ok = false;
    if (n) {
        ok = mp_service_post_begin("/v1/report", body, n, kReportTimeoutMs);
        mp_log("diag: report %s kind=%s where=%s body=%u bytes log=%u bytes", ok ? "sent" : "NOT started", in.kind, in.where, (unsigned)n, (unsigned)logBytes);
    }
    free(body);
    if (!ok) return false;
    g_rep.active = true;
    g_rep.startMs = mp_sys_now_ms();
    g_rep.timeoutMs = kReportTimeoutMs;
    return true;
}

extern "C" int mp_diag_report_poll(char* idOut, int cap) {
    DIAG_HOST_ALLOC;
    if (idOut && cap > 0) idOut[0] = 0;
    if (!g_rep.active) return MP_DIAG_FAILED;
    int transport = 2, status = 0;
    char body[512];
    const int r = mp_service_post_poll(&transport, &status, body, sizeof body);
    if (r == 0) return MP_DIAG_PENDING;
    g_rep.active = false;
    if (r < 0) return MP_DIAG_FAILED;
    int res = MP_DIAG_FAILED;
    if (transport == 1) res = MP_DIAG_OFFLINE;
    else if (transport == 3) res = MP_DIAG_CLOCK;
    else if (transport == 2) res = mp_sys_now_ms() - g_rep.startMs + 1500 >= g_rep.timeoutMs ? MP_DIAG_TIMEOUT : MP_DIAG_FAILED;
    else if (status == 201 || status == 200) {
        res = MP_DIAG_OK;
        // {"id":"XXXXXXXX"}
        const char* k = strstr(body, "\"id\"");
        if (k && idOut && cap > 0) {
            k = strchr(k + 4, '"');
            if (k) {
                ++k;
                int n = 0;
                while (*k && *k != '"' && n < cap - 1 && n < 16) {
                    const char c = *k++;
                    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) idOut[n++] = c;
                }
                idOut[n] = 0;
            }
        }
    } else if (status == 400) res = MP_DIAG_BAD_REQUEST;
    else if (status == 413) res = MP_DIAG_TOO_LARGE;
    else if (status == 429) res = MP_DIAG_RATE_LIMITED;
    else if (status >= 500) res = MP_DIAG_BUSY;
    mp_log("diag: report answer status=%d result=%d", status, res);
    return res;
}

extern "C" void mp_diag_report_cancel(void) {
    DIAG_HOST_ALLOC;
    if (g_rep.active) mp_service_post_cancel();
    g_rep.active = false;
}
