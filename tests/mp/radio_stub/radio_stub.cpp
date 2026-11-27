// Host stand-in for the Vita ad hoc API (psp2/pspnet_adhoc*, netctl, netcheck dialog, sysmodule, a few kernel calls): our own code, written from the
// API shape only. Model: a "cell" in shared memory; station N has MAC 02:00:00:00:00:NN and a UDP loopback socket; a PDP datagram is a small header
// (kind, source port, destination port, source MAC) plus the payload. See radio_stub.h for the fault switches.
#include "radio_stub.h"
#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/netcheck_dialog.h>
#include <psp2/pspnet_adhoc.h>
#include <psp2/pspnet_adhocctl.h>
#include <psp2/sysmodule.h>

#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>
#include <new>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace radiostub;

namespace {
Cell *g_cell = nullptr; int g_me = 0;
Station &me() { return g_cell->st[g_me]; }
struct Local {
    bool net{}, netctl{}, adhoc{}, ctl{}, appUtil{}, dialogCfg{}; bool mods[0x80]{};
    int sock = -1; int pdpId = 0; uint16_t port = 0;
    int dlgState = 0, dlgResult = 0; uint64_t dlgStart = 0;   // 0 none, 1 running, 2 finished
    int ctlRefusals = 0;
} L;
const int kBadId = 0;
constexpr int kHdr = 12;
struct Hdr { uint8_t kind, pad; uint16_t sport, dport; uint8_t src[6]; };   // kind: 1 unicast, 2 broadcast
uint64_t nowUs() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return uint64_t(ts.tv_sec) * 1000000u + ts.tv_nsec / 1000; }
const uint64_t g_t0 = nowUs();
// "fail next n": counts down; -1 = for ever
bool take(std::atomic<int> &n) {
    int v = n.load();
    while (v != 0) { if (v < 0) return true; if (n.compare_exchange_weak(v, v - 1)) return true; }
    return false;
}
bool vanished() { return g_cell && me().f.vanish.load() != 0; }
bool is_broadcast(const uint8_t *m) { for (int i = 0; i < 6; ++i) if (m[i] != 0xFF) return false; return true; }
}

namespace radiostub {
Cell *create_cell(int stations) {
    void *m = mmap(nullptr, sizeof(Cell), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    Cell *c = new (m) Cell();
    c->stations = stations;
    // a base port whose whole range is free right now (parallel runs, other programs): probe, else try the next block
    for (int tries = 0, base = 30000 + int((getpid() * 2654435761u) % 20000); ; ++tries, base = 30000 + (base - 30000 + 16) % 20000) {
        bool free_all = true; int fds[kMaxStations + 1]; int opened = 0;
        for (int i = 1; i <= stations && free_all; ++i) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0); sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(uint16_t(base + i));
            if (fd < 0 || bind(fd, (sockaddr *)&a, sizeof a) < 0) free_all = false;
            if (fd >= 0) fds[opened++] = fd;
        }
        for (int i = 0; i < opened; ++i) close(fds[i]);
        if (free_all) {   // claim the block with a lock file held for the life of the test process (and its forks): two runs cannot pick the same block
            char path[64]; snprintf(path, sizeof path, "/tmp/radio_stub_port_%d.lock", base);
            const int lk = open(path, O_CREAT | O_RDWR, 0666);
            if (lk < 0 || flock(lk, LOCK_EX | LOCK_NB) < 0) { if (lk >= 0) close(lk); free_all = false; }
        }
        c->base_port = base; if (free_all || tries > 200) break;
    }
    return c;
}
void mac_of(int station, uint8_t out[6]) { const uint8_t m[6] = {2, 0, 0, 0, 0, uint8_t(station)}; memcpy(out, m, 6); }
std::string log_of(Cell *c, int s) { return std::string(c->st[s].log, c->st[s].log_len.load()); }
bool log_has(Cell *c, int s, const char *n) { return log_of(c, s).find(n) != std::string::npos; }
void attach(Cell *cell, int station) {
    g_cell = cell; g_me = station;
    if (const char *e = getenv("SMG_STUB_FAULTS")) {
        Faults &f = me().f; std::string s(e); size_t i = 0;
        while (i <= s.size()) {
            size_t j = s.find(',', i); if (j == std::string::npos) j = s.size();
            std::string t = s.substr(i, j - i); i = j + 1;
            const size_t eq = t.find('='); const std::string k = t.substr(0, eq); const int v = eq == std::string::npos ? 1 : atoi(t.c_str() + eq + 1);
            if (k == "bcast") f.broadcast_broken = 1; else if (k == "swap") f.swap_sport = 1; else if (k == "zero") f.zero_mac = 1;
            else if (k == "fb") f.firstbyte_mac = 1; else if (k == "err") f.recv_err_burst = v; else if (k == "dis") f.disconnect_fail = -1;
            else if (k == "ctlterm") f.ctlterm_fail = -1; else if (k == "adhocterm") f.adhocterm_fail = -1; else if (k == "pdpdel") f.pdpdelete_fail = -1;
            else if (k == "keep") f.term_keeps_state = 1; else if (k == "vanish") f.vanish = 1; else if (k == "ms") f.dialog_ms = v;
            else if (k == "refgroup") f.dialog_refuse_group = 1; else if (k == "unload") f.module_unload_fail = -1; else if (k == "oversize") f.peerlist_oversize = v;
            else if (k == "dlg") f.dialog_result = t.find("cancel") != std::string::npos ? 1 : 2;
        }
    }
}
}

// The radio's NpLogf mirrors every line here on the "Vita" (np_log.h calls port_log_summary when it exists): the test reads them back.
extern "C" void port_log_summary(const char *fmt, ...) {
    if (!g_cell) return;
    char line[512]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line - 1, fmt, ap); va_end(ap);
    Station &s = me(); const uint32_t n = s.log_len.load(); const size_t len = strlen(line);
    if (n + len + 1 >= sizeof s.log) return;
    memcpy(s.log + n, line, len); s.log[n + len] = '\n'; s.log_len.store(n + uint32_t(len) + 1);
}

extern "C" {
// ---- kernel ----
uint64_t sceKernelGetProcessTimeWide(void) { return nowUs() - g_t0 + 1; }
int sceKernelDelayThread(SceUInt d) { usleep(d); return 0; }
struct StubThread { pthread_t t; SceKernelThreadEntry entry; int started; std::atomic<int> done; char name[32]; };
static StubThread *g_threads[16];
static void *thread_main(void *a) { StubThread *t = (StubThread *)a; t->entry(0, nullptr); t->done = 1; return nullptr; }
SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int, SceSize, SceUInt, int, const void *) {
    for (int i = 0; i < 16; ++i) if (!g_threads[i]) {
        StubThread *t = new StubThread(); t->entry = entry; t->started = 0; t->done = 0; snprintf(t->name, sizeof t->name, "%s", name); g_threads[i] = t; return 0x40000 + i;
    }
    return (int)0x80020001;
}
static StubThread *thr(SceUID id) { const int i = id - 0x40000; return i >= 0 && i < 16 ? g_threads[i] : nullptr; }
int sceKernelStartThread(SceUID id, SceSize, void *) { StubThread *t = thr(id); if (!t) return (int)0x80020001; t->started = 1; return pthread_create(&t->t, nullptr, thread_main, t) ? (int)0x80020001 : 0; }
int sceKernelWaitThreadEnd(SceUID id, int *, SceUInt *timeout) {
    StubThread *t = thr(id); if (!t) return (int)0x80020001;
    const uint64_t until = nowUs() + (timeout ? *timeout : 0xFFFFFFFFu);
    while (t->started && !t->done) { if (nowUs() >= until) return SCE_KERNEL_ERROR_WAIT_TIMEOUT; usleep(200); }
    return 0;
}
int sceKernelDeleteThread(SceUID id) {
    StubThread *t = thr(id); if (!t) return (int)0x80020001;
    if (t->started) { if (!t->done) return (int)0x80020002; pthread_join(t->t, nullptr); }
    delete t; g_threads[id - 0x40000] = nullptr; return 0;
}
int sceKernelGetThreadInfo(SceUID, SceKernelThreadInfo *i) { i->size = sizeof *i; snprintf(i->name, sizeof i->name, "stub_main"); return 0; }
int sceKernelGetThreadId(void) { return 0x40100; }

// ---- sysmodule / app ----
int sceSysmoduleIsLoaded(SceSysmoduleModuleId id) { return L.mods[id & 0x7F] ? SCE_SYSMODULE_LOADED : (int)0x805A1001; }
int sceSysmoduleLoadModule(SceSysmoduleModuleId id) { L.mods[id & 0x7F] = true; return 0; }
int sceSysmoduleUnloadModule(SceSysmoduleModuleId id) { if (take(me().f.module_unload_fail)) return (int)0x805A1002; L.mods[id & 0x7F] = false; return 0; }
int sceAppUtilInit(SceAppUtilInitParam *, SceAppUtilBootParam *) { if (L.appUtil) return SCE_APPUTIL_ERROR_BUSY; L.appUtil = true; return 0; }
int sceAppUtilSystemParamGetInt(unsigned id, int *v) { *v = id == SCE_SYSTEM_PARAM_ID_LANG ? 1 : 1; return 0; }
int sceAppMgrAppParamGetString(int, int, char *s, SceSize n) { snprintf(s, n, "PCSG90000"); return 0; }
int sceCommonDialogSetConfigParam(const SceCommonDialogConfigParam *) { L.dialogCfg = true; return 0; }

// ---- sceNet / netctl ----
int sceNetInit(SceNetInitParam *) { if (L.net) return SCE_NET_ERROR_EBUSY; L.net = true; return 0; }
int sceNetTerm(void) { L.net = false; return 0; }
int sceNetCtlInit(void) { if (L.netctl) return (int)0x80412102; L.netctl = true; return 0; }
void sceNetCtlTerm(void) { L.netctl = false; }
int sceNetCtlAdhocGetState(int *state) {
    if (!L.netctl) return (int)0x80412101;
    *state = (!vanished() && me().joined.load()) ? SCE_NETCTL_STATE_CONNECTED : SCE_NETCTL_STATE_DISCONNECTED; return 0;
}
int sceNetCtlAdhocDisconnect(void) {
    if (take(me().f.disconnect_fail)) { if (!me().f.term_keeps_state) me().joined = 0; return (int)0x80412103; }
    me().joined = 0; return 0;
}

// ---- adhoc ----
int sceNetAdhocInit(void) {
    if (L.adhoc) return SCE_ERROR_NET_ADHOC_ALREADY_INITIALIZED;
    L.adhoc = true; me().adhoc_inits++; return 0;
}
int sceNetAdhocTerm(void) {
    if (!L.adhoc) return SCE_ERROR_NET_ADHOC_NOT_INITIALIZED;
    if (take(me().f.adhocterm_fail)) { if (!me().f.term_keeps_state) L.adhoc = false; return (int)0x80410750; }
    L.adhoc = false; return 0;
}
int sceNetAdhocctlInit(const SceNetAdhocctlAdhocId *id) {
    if (id && me().f.ctl_refuse_smg && !memcmp(id->data, "SMGV00001", 9)) return SCE_ERROR_NET_ADHOCCTL_INVALID_ARG;
    if (L.ctl) return SCE_ERROR_NET_ADHOCCTL_ALREADY_INITIALIZED;
    if (!id || id->type > 2) return SCE_ERROR_NET_ADHOCCTL_INVALID_ARG;
    if (me().f.ctl_refuse_first && L.ctlRefusals++ == 0) return SCE_ERROR_NET_ADHOCCTL_INVALID_ARG;
    L.ctl = true; me().ctl_inits++; return 0;
}
int sceNetAdhocctlTerm(void) {
    if (!L.ctl) return SCE_ERROR_NET_ADHOCCTL_NOT_INITIALIZED;
    if (take(me().f.ctlterm_fail)) { if (!me().f.term_keeps_state) { L.ctl = false; me().joined = 0; } return (int)0x80410B10; }
    L.ctl = false; me().joined = 0; return 0;
}
int sceNetAdhocctlGetEtherAddr(SceNetEtherAddr *a) {
    if (!L.ctl) return SCE_ERROR_NET_ADHOCCTL_NOT_INITIALIZED;
    if (me().f.zero_mac) memset(a->data, 0, 6); else mac_of(g_me, a->data);
    return 0;
}
int sceNetAdhocctlGetPeerList(int *buflen, void *buf) {
    if (!L.ctl) return SCE_ERROR_NET_ADHOCCTL_NOT_INITIALIZED;
    if (vanished() || !me().joined) return (int)0x80410B0C;
    int ids[kMaxStations]; int n = 0;
    for (int i = 1; i <= g_cell->stations; ++i) if (i != g_me && g_cell->st[i].joined.load() && !g_cell->st[i].f.vanish.load()) ids[n++] = i;
    const int each = (int)sizeof(SceNetAdhocctlPeerInfo);
    if (!buf) { *buflen = n * each; return 0; }
    const int room = *buflen / each; const int put = n < room ? n : room;
    SceNetAdhocctlPeerInfo *p = (SceNetAdhocctlPeerInfo *)buf;
    for (int i = 0; i < put; ++i) { memset(&p[i], 0, sizeof p[i]); mac_of(ids[i], p[i].macAddr.data); p[i].next = i + 1 < put ? &p[i + 1] : nullptr; }
    *buflen = put * each + me().f.peerlist_oversize.load();   // oversize != 0: a firmware that claims more than it wrote (the radio must not trust it)
    me().peers_seen_by_me = put;
    return 0;
}
int sceNetAdhocPdpCreate(const SceNetEtherAddr *saddr, SceUShort16 sport, unsigned, int) {
    if (!L.adhoc) return SCE_ERROR_NET_ADHOC_NOT_INITIALIZED;
    if (!saddr) return SCE_ERROR_NET_ADHOC_INVALID_ADDR;
    if (L.sock >= 0) return SCE_ERROR_NET_ADHOC_PORT_IN_USE;
    int fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return (int)0x80410740;
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(uint16_t(g_cell->base_port + g_me));
    if (bind(fd, (sockaddr *)&a, sizeof a) < 0) { close(fd); return SCE_ERROR_NET_ADHOC_PORT_IN_USE; }
    L.sock = fd; L.port = sport; L.pdpId = 1; me().pdp_creates++; return L.pdpId;
}
int sceNetAdhocPdpDelete(int id, int) {
    if (!L.adhoc) return SCE_ERROR_NET_ADHOC_NOT_INITIALIZED;
    if (id != L.pdpId || L.sock < 0) return SCE_ERROR_NET_ADHOC_INVALID_SOCKET_ID;
    if (take(me().f.pdpdelete_fail)) { close(L.sock); L.sock = -1; return (int)0x80410760; }   // (the socket is gone either way)
    close(L.sock); L.sock = -1; return 0;
}
int sceNetAdhocPdpSend(int id, const SceNetEtherAddr *d, SceUShort16 dport, const void *data, int len, unsigned, int) {
    if (!L.adhoc) return SCE_ERROR_NET_ADHOC_NOT_INITIALIZED;
    if (id != L.pdpId || L.sock < 0) return SCE_ERROR_NET_ADHOC_INVALID_SOCKET_ID;
    if (!d || !data) return SCE_ERROR_NET_ADHOC_INVALID_ADDR;
    if (len <= 0 || len > SCE_NET_ADHOC_PDP_MFS) return SCE_ERROR_NET_ADHOC_INVALID_DATALEN;
    if (!me().joined.load()) return SCE_ERROR_NET_ADHOC_NOT_CONNECTED;
    if (vanished()) return len;   // a suspended console: nothing leaves it
    uint8_t buf[kHdr + SCE_NET_ADHOC_PDP_MFS]; Hdr h{}; const bool bc = is_broadcast(d->data);
    h.kind = bc ? 2 : 1; h.sport = L.port; h.dport = dport;
    if (me().f.zero_mac) memset(h.src, 0, 6); else { mac_of(g_me, h.src); if (me().f.firstbyte_mac) h.src[0] ^= 0x40; }
    memcpy(buf, &h, kHdr); memcpy(buf + kHdr, data, len);
    auto to = [&](int st) {
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(uint16_t(g_cell->base_port + st));
        sendto(L.sock, buf, kHdr + len, 0, (sockaddr *)&a, sizeof a);
    };
    if (bc) {
        me().tx_bcast++;
        if (me().f.broadcast_broken) return len;
        for (int i = 1; i <= g_cell->stations; ++i) if (i != g_me && g_cell->st[i].joined.load() && !g_cell->st[i].f.vanish.load()) to(i);
        return len;
    }
    me().tx_ucast++;
    for (int i = 1; i <= g_cell->stations; ++i) {
        uint8_t m[6]; mac_of(i, m);
        if (i != g_me && !memcmp(m, d->data, 6) && g_cell->st[i].joined.load() && !g_cell->st[i].f.vanish.load()) { to(i); return len; }
    }
    return len;   // an address nobody owns: dropped on the air, the sender is told nothing
}
int sceNetAdhocPdpRecv(int id, SceNetEtherAddr *saddr, SceUShort16 *sport, void *buf, int *len, unsigned timeout, int flag) {
    if (!L.adhoc) return SCE_ERROR_NET_ADHOC_NOT_INITIALIZED;
    if (id != L.pdpId || L.sock < 0) return SCE_ERROR_NET_ADHOC_INVALID_SOCKET_ID;
    if (take(me().f.recv_err_burst)) { me().rx_errs_served++; return me().f.recv_err_code.load(); }
    const uint64_t until = nowUs() + ((flag & SCE_NET_ADHOC_F_NONBLOCK) ? 0 : timeout);
    for (;;) {
        const uint64_t t = nowUs(); const int wait_ms = t >= until ? 0 : int((until - t + 999) / 1000);
        pollfd pf{L.sock, POLLIN, 0};
        if (poll(&pf, 1, wait_ms) <= 0) return (flag & SCE_NET_ADHOC_F_NONBLOCK) ? SCE_ERROR_NET_ADHOC_WOULD_BLOCK : SCE_ERROR_NET_ADHOC_TIMEOUT;
        uint8_t in[kHdr + SCE_NET_ADHOC_PDP_MFS + 64];
        const ssize_t n = recv(L.sock, in, sizeof in, 0);
        if (n < kHdr || vanished()) continue;
        Hdr h; memcpy(&h, in, kHdr);
        if (h.dport != L.port) continue;
        memcpy(saddr->data, h.src, 6);
        *sport = me().f.swap_sport ? uint16_t((h.sport >> 8) | (h.sport << 8)) : h.sport;
        int pay = int(n) - kHdr; if (pay > *len) pay = *len;
        memcpy(buf, in + kHdr, pay); *len = pay;
        if (h.kind == 2) me().rx_bcast++; else me().rx_ucast++;
        return 0;
    }
}

// ---- NetCheck dialog (the dialog is "drawn" by nothing; it finishes after dialog_ms with the scripted result) ----
SceInt32 sceNetCheckDialogInit(SceNetCheckDialogParam *p) {
    if (!L.dialogCfg) return SCE_COMMON_DIALOG_ERROR_NOT_CONFIGURED;
    if (L.dlgState) return SCE_COMMON_DIALOG_ERROR_BUSY;
    Station &s = me(); s.dlg_inits++;
    if (take(s.f.dialog_init_fail) || (p->groupName && s.f.dialog_refuse_group)) return (int)0x80100C07;
    s.dlg_group = p->groupName != nullptr; s.dlg_mode = p->mode; s.dlg_timeout_us = p->timeoutUs;
    memset(s.dlg_group_name, 0, 9); if (p->groupName) memcpy(s.dlg_group_name, p->groupName->data, 8);
    memset(s.dlg_id, 0, 10); memcpy(s.dlg_id, p->npCommunicationId.data, 9);
    L.dlgState = 1; L.dlgStart = nowUs(); return 0;
}
static void dialog_settle() {
    if (L.dlgState != 1) return;
    Station &s = me(); const uint64_t ms = (nowUs() - L.dlgStart) / 1000;
    const uint32_t tmo = s.dlg_timeout_us.load();
    if (tmo && ms * 1000 >= tmo && uint64_t(s.f.dialog_ms.load()) * 1000 >= tmo) { L.dlgState = 2; L.dlgResult = (int)0x80100C06; return; }   // the dialog's own time-out
    if (ms < uint64_t(s.f.dialog_ms.load())) return;
    L.dlgState = 2; L.dlgResult = s.f.dialog_result.load() == 0 ? SCE_COMMON_DIALOG_RESULT_OK : s.f.dialog_result.load() == 1 ? SCE_COMMON_DIALOG_RESULT_USER_CANCELED : (int)0x80100C06;
    if (L.dlgResult == SCE_COMMON_DIALOG_RESULT_OK) s.joined = 1;
}
SceCommonDialogStatus sceNetCheckDialogGetStatus(void) { dialog_settle(); return (SceCommonDialogStatus)L.dlgState; }
SceInt32 sceNetCheckDialogAbort(void) {
    if (L.dlgState != 1) return SCE_COMMON_DIALOG_ERROR_NOT_CONFIGURED + 1;   // NOT_RUNNING
    L.dlgState = 2; L.dlgResult = SCE_COMMON_DIALOG_RESULT_ABORTED; return 0;
}
SceInt32 sceNetCheckDialogGetResult(SceNetCheckDialogResult *r) {
    dialog_settle(); if (L.dlgState != 2) return SCE_COMMON_DIALOG_ERROR_NOT_FINISHED;
    memset(r, 0, sizeof *r); r->result = L.dlgResult; return 0;
}
SceInt32 sceNetCheckDialogTerm(void) { if (!L.dlgState) return SCE_COMMON_DIALOG_ERROR_NOT_IN_USE; L.dlgState = 0; return 0; }
}
