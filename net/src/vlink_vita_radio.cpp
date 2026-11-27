#if defined(NP_TARGET_VITA)  // the Vita target only (the np Makefile compiles every platform/ source for every target)

#include "vlink_vita_radio.h"
#include "np_log.h"   // Galaxy: one-line events also reach the Vita log.txt

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

namespace {
constexpr unsigned kPort = 19021, kRxSlots = 256, kNetPool = NP_NET_POOL_BYTES;
// Raw records carry one PDP datagram: at most SCE_NET_ADHOC_PDP_MFS (1444) bytes. The link above splits longer frames (vlink.h Backend::maxFrame);
// a send longer than the limit is refused here and counted (np_vita_radio_snapshot: oversize), never handed to the firmware. The receive side
// accepts up to 1500 as before (nothing longer than the MFS can arrive).
constexpr unsigned kRawMax = 1500;
std::atomic<uint32_t> g_oversizeSends{0};
enum Step { MODULE_NET, MODULE_ADHOC, POOL, NET, NETCTL, ADHOC, CTL,
            DIALOG_INIT, DIALOG_POLL, DIALOG_RESULT, DIALOG_TERM,
            MAC, SOCKET, RING, THREAD_CREATE, THREAD_START, OPENING, ACTIVE };
const char *const names[] = {"module_net", "module_adhoc", "pool", "net", "netctl", "adhoc", "ctl",
    "dialog", "dialog_poll", "dialog_result", "dialog_term", "mac", "socket", "ring", "thread", "thread_start", "open", "active"};
enum class DialogOp { Init, Status, Result, Abort, Term };
enum class MailState { Idle, Requested, Completed };
struct DialogReply { DialogOp op; int rc, result; };
struct DialogMailbox {
    std::atomic<MailState> state{MailState::Idle};
    DialogReply reply{};
} mail;
struct RawRecord { NpRadioAddress address; uint16_t length; uint8_t bytes[kRawMax]; };
struct Platform {
    NpRadioPhase phase{NP_RADIO_DOWN}; NpRadioStatus status{NP_RADIO_OK}; Step step{MODULE_NET};
    bool host{}, module_net{}, module_adhoc{}, net{}, netctl{}, adhoc{}, ctl{}, dialog{}, aborted{}, result_read{}, worker_started{}, cell{};
    DialogOp close_dialog{DialogOp::Status};
    bool cleanup_stalled{};
    unsigned cleanup_failures{};
    uint64_t cleanup_wait_us{}, cleanup_retry_us{};
    const char *cleanup_stage{"close"};
    uint32_t error{};          // why the radio closed (what the player's log should show), kept through the teardown
    uint32_t cleanup_error{};  // the last teardown call's own failure
    void *pool{}; RawRecord *ring{}; SceUID worker{-1}; int socket{-1};
    SceNetEtherAddr mac{};
    SceNetCheckDialogParam dialog_param{}; SceNetAdhocctlGroupName group{};
    std::atomic<bool> run{false}; std::atomic<uint32_t> rx_error{0}, head{0}, tail{0}, dropped{0};
} p;
std::atomic<bool> dialog_visible{false};
std::atomic<bool> dialog_ready{false};

// ---- ad hoc ids (wave 5 NET) ------------------------------------------------------------------------------------------------------
// Galaxy: every package uses the ad hoc id SMGV00001 (also its title id) and the group SMGMP001 (Strikers: MSCH00002 / MSCMP001),
// so any two of them find each other. The two shipping references that work on hardware (VitaShell, vitaQuake) pass their own title id
// and no group name, and nobody knows whether the firmware insists on that: so when sceNetAdhocctlInit or the NetCheck dialog refuses
// the first choice, the next one is tried (the running title id; no group name), and the log says which one worked. A console that
// had to fall back to its own title id finds only consoles running the same package.
const char kAdhocId[] = "SMGV00001";   // Galaxy (Strikers: MSCH00002)
const char kGroup[] = "SMGMP001";      // Galaxy (Strikers: MSCMP001)
char g_title[16] = {};      // the running title id (sceAppMgrAppParamGetString), or empty
const char* g_ctlId = kAdhocId;
const char* g_ctlInitId = nullptr;   // the id sceNetAdhocctlInit last ACCEPTED (>= 0); what a library left initialised by a skipped Term was given
int g_dialogVariant = 0;    // 0: id + group, 1: id without a group, 2: the title id without a group
uint32_t g_dialogFirstError = 0, g_ctlFirstError = 0;
bool g_idLogged = false;
const char* TitleId() {
    if (!g_title[0]) {
        char t[16] = {};
        if (sceAppMgrAppParamGetString(0, 12, t, sizeof t) >= 0 && std::strlen(t) == 9) std::memcpy(g_title, t, 10);
    }
    return g_title[0] ? g_title : nullptr;
}

// ---- link watch (wave 5 NET, SMG_LAN_LINKWATCH, default 1) -------------------------------------------------------------------------
// The cell is looked at once a second and the link ends after two looks in a row say it is gone (as VitAdhoc does); 0 = the earlier
// behaviour: every presented frame, at the first look. Also: a ring high-water mark for the log, and a note when no nearby console has
// been seen for 30 s (two consoles that both created their own cell never see each other: NEARBY HOST on one, NEARBY JOIN on the other).
bool LinkWatch() {
    static const int on = [] { const char* v = std::getenv("SMG_LAN_LINKWATCH"); return v && v[0] == '0' ? 0 : 1; }();
    return on != 0;
}
// ---- console-session switches (env.txt, read when a Nearby session starts; defaults = the behaviour before the switches existed) ----------
// SMG_LAN_DIALOG=conn|create|join: the NetCheck dialog mode (default conn; any other text means join, as before).
// SMG_LAN_GROUP=1|0: 1 = the NetCheck dialog gets the group name SMGMP001 (first choice, as before); 0 = no group name (starts at the "id without a
// group" choice). SMG_LAN_TIMEOUT_MS=n: the NetCheck dialog's own time-out in milliseconds (default 60000; clamped to 0..3600000; text that is not a
// number = the default; 0 is passed to the system as 0, which is its "no time-out" as far as the SDK says; the player's cancel and a suspend still end
// the dialog through the Abort path). Read once per session by np_vita_radio_begin_link (the dialog thread only reads the copies); one log line.
int g_lanGroup = 1, g_lanMode = 0;   // g_lanMode: 0 conn, 1 create, 2 join
uint32_t g_lanTimeoutMs = 60000;
const char *const kModeName[] = {"conn", "create", "join"};
long EnvNumber(const char *name, long fallback, long lo, long hi) {
    const char *v = std::getenv(name);
    if (!v || !*v) return fallback;
    char *end = nullptr; const long n = std::strtol(v, &end, 0);
    if (!end || *end || n < 0) return fallback;   // (text, or a negative number: the default)
    return n < lo ? lo : n > hi ? hi : n;
}
std::atomic<uint32_t> g_ringPeak{0};
std::atomic<uint32_t> g_rxLateMax{0};   // Galaxy (MP3 nocap): see receive_worker
std::atomic<uint32_t> g_txOk{0}, g_txBlocked{0}, g_txErr{0}, g_txBlockedAll{0}, g_txErrAll{0};   // Galaxy (netlog): sends the ad hoc radio took / turned away as transient / failed (np_vita_radio_send_take)
// Galaxy (adhoc review): what the receive worker saw that the owner thread logs once (the worker does not log): the first PdpRecv error that
// is not a time-out, how many there were, and the first source port that is not kPort (0x10000 | port; the byte order question, H3).
std::atomic<uint32_t> g_rxFirstErr{0}, g_rxErrCount{0}, g_rxOddPort{0};
bool g_rxErrLogged = false, g_rxPortLogged = false;
// Galaxy (adhoc review): the first 8 distinct sender MACs; the receive worker appends (and publishes the count), the pump logs and checks them.
uint8_t g_seenMac[8][6]; std::atomic<uint32_t> g_seenCount{0}; uint32_t g_seenLogged = 0;
uint64_t g_openUs = 0, g_nextLookUs = 0;
int g_cellStrikes = 0;
bool g_peerSeen = false, g_noPeerNoted = false;
// AppUtil/config belong to the process-lifetime renderer, not each radio session.
bool renderer_apputil{}, renderer_config{};

void cleanup_progress() {
    p.cleanup_wait_us = sceKernelGetProcessTimeWide(); p.cleanup_retry_us = 0;
    p.cleanup_failures = 0; p.cleanup_stalled = false;
}
// Galaxy (adhoc review, advised 3): a teardown call that was given up on may leave the ad hoc stack half up; the Same Wi-Fi and online paths log it
// when they open (np_vita_radio_note_leftovers) so a console log that shows odd Wi-Fi errors says why. Cleared by the next close that skipped nothing.
const char *g_leftovers = nullptr; bool g_skippedThisClose = false;
bool cleanup_failed(int rc) {
    if (rc >= 0) return false;
    p.cleanup_error = static_cast<uint32_t>(rc);   // (Galaxy: the close reason in p.error is not overwritten by a failing teardown call)
    if (++p.cleanup_failures >= 8) p.cleanup_stalled = true;
    p.cleanup_retry_us = sceKernelGetProcessTimeWide() + (p.cleanup_stalled ? 1000000 : 100000);
    return true;
}

// Galaxy (adhoc review): a teardown call that may fail because the cell is already gone (Disconnect, PdpDelete, the two Terms after a lost
// cell). Strikers retried it for ever (the radio stayed CLOSING, no new session could start); VitAdhoc ignores the result. Three tries, then
// the failure is logged and the close goes on.
bool cleanup_soft(int rc, const char *what) {
    if (rc >= 0) return false;
    cleanup_failed(rc);
    if (p.cleanup_failures < 3) return true;
    NpLogf("vlink-vita", "%s failed 0x%08X three times: going on with the close\n", what, static_cast<unsigned>(rc));
    g_leftovers = what; g_skippedThisClose = true;
    cleanup_progress(); return false;
}

// One owner publishes stable storage, one renderer replies. Neither can reuse
// the mailbox until the owner acquires completion, even during cancellation.
void request_dialog(DialogOp op) {
    mail.reply = {op, 0, 0};
    mail.state.store(MailState::Requested, std::memory_order_release);
}
DialogReply consume_dialog() {
    const DialogReply reply = mail.reply;
    if (reply.rc >= 0) {
        switch (reply.op) {
        case DialogOp::Init: p.dialog = true; break;
        case DialogOp::Result:
            p.result_read = true;
            if (reply.result == SCE_COMMON_DIALOG_RESULT_OK) p.cell = true;
            break;
        case DialogOp::Abort: p.aborted = true; break;
        case DialogOp::Term: p.dialog = false; break;
        case DialogOp::Status: break;
        }
    }
    mail.state.store(MailState::Idle, std::memory_order_release);
    return reply;
}
bool call_dialog(DialogOp op, DialogReply &reply) {
    const auto state = mail.state.load(std::memory_order_acquire);
    if (state == MailState::Idle) { request_dialog(op); return false; }
    if (state != MailState::Completed) return false;
    reply = consume_dialog(); return true;
}
void cleanup_dialog() {
    const auto state = mail.state.load(std::memory_order_acquire);
    if (state == MailState::Requested) return;
    if (state == MailState::Idle) {
        if (p.dialog) request_dialog(p.close_dialog);
        return;
    }
    const auto reply = consume_dialog();
    if (reply.rc < 0) {
        cleanup_failed(reply.rc);
        p.close_dialog = reply.op == DialogOp::Init ? DialogOp::Status : reply.op;
        return;
    }
    switch (reply.op) {
    case DialogOp::Status:
        p.close_dialog = reply.rc == SCE_COMMON_DIALOG_STATUS_RUNNING ?
            (p.aborted ? DialogOp::Status : DialogOp::Abort) :
            (reply.rc == SCE_COMMON_DIALOG_STATUS_FINISHED && !p.result_read ? DialogOp::Result : DialogOp::Term);
        break;
    case DialogOp::Result: p.close_dialog = DialogOp::Term; break;
    case DialogOp::Init:
    case DialogOp::Abort:
    case DialogOp::Term: p.close_dialog = DialogOp::Status; break;
    }
    if (reply.op != DialogOp::Status) cleanup_progress();
}

void close_with(NpRadioStatus status, uint32_t error = 0) {
    if (p.phase == NP_RADIO_DOWN || p.phase == NP_RADIO_CLOSING) return;
    p.status = status; p.error = error; p.cleanup_error = 0; g_skippedThisClose = false; p.phase = NP_RADIO_CLOSING;
    p.cleanup_stage = "close"; cleanup_progress();
    p.run.store(false, std::memory_order_release);
}
bool failed(int rc) {
    if (rc >= 0) return false;
    close_with(NP_RADIO_CLOSED, static_cast<uint32_t>(rc)); return true;
}
// Galaxy (MP3 nocap): the receive worker's priority (see THREAD_CREATE).
int rx_priority() {
    const char *e = std::getenv("SMG_MP_RX_PRIO");
    if (!e || !*e) return 0x10000100;
    char *end = nullptr;
    const long v = std::strtol(e, &end, 0);
    return end && !*end && v >= 0x40 && v <= 0xBF ? static_cast<int>(v) : 0x10000100;
}
bool transient(int rc) {
    return static_cast<uint32_t>(rc) == SCE_ERROR_NET_ADHOC_WOULD_BLOCK ||
           static_cast<uint32_t>(rc) == SCE_ERROR_NET_ADHOC_TIMEOUT;
}
int receive_worker(SceSize, void *) {
    uint8_t scratch[1472];
    unsigned errRun = 0;   // consecutive PdpRecv errors that are not time-outs
    while (p.run.load(std::memory_order_acquire)) {
        SceNetEtherAddr from{}; unsigned short port{}; int length = sizeof(scratch);
        const uint64_t before = sceKernelGetProcessTimeWide();
        int rc = sceNetAdhocPdpRecv(p.socket, &from, &port, scratch, &length, 10000, 0);
        if (!p.run.load(std::memory_order_acquire)) break;
        if (rc < 0) {
            if (!transient(rc)) {
                // Galaxy (adhoc review): one odd error code no longer ends the link (VitAdhoc counts it and goes on); about a second of
                // nothing but errors (200 x 5 ms) still does. The first code is logged by the pump ("PdpRecv error").
                uint32_t none = 0;
                g_rxFirstErr.compare_exchange_strong(none, static_cast<uint32_t>(rc), std::memory_order_relaxed);
                g_rxErrCount.fetch_add(1, std::memory_order_relaxed);
                if (++errRun >= 200) { p.rx_error.store(static_cast<uint32_t>(rc), std::memory_order_release); break; }
                sceKernelDelayThread(5000);
                continue;
            }
            errRun = 0;
            // Galaxy (MP3 nocap): a timed-out wait that came back late = how long this thread waited for its core (core 2, under the
            // front end's player at default priority): the "[mp] net" line's "rx late max" (np_vita_radio_rx_late_take_max).
            if (static_cast<uint32_t>(rc) == SCE_ERROR_NET_ADHOC_TIMEOUT) {
                const uint64_t took = sceKernelGetProcessTimeWide() - before;
                const uint32_t late = took > 10000 ? static_cast<uint32_t>(took - 10000 > 0xFFFFFFFFu ? 0xFFFFFFFFu : took - 10000) : 0u;
                if (late > g_rxLateMax.load(std::memory_order_relaxed)) g_rxLateMax.store(late, std::memory_order_relaxed);
            }
            // A firmware/backend returning immediately must not spin a core.
            if (sceKernelGetProcessTimeWide() - before < 1000) sceKernelDelayThread(1000);
            continue;
        }
        errRun = 0;
        if (rc > 0 && length == static_cast<int>(sizeof(scratch))) length = rc;
        const uint32_t head = p.head.load(std::memory_order_relaxed);
        const uint32_t tail = p.tail.load(std::memory_order_acquire);
        // Galaxy (adhoc review): no filter on the source port: our socket only receives datagrams sent to kPort, and a source port that came
        // back byte-swapped (H3) would have dropped every frame. The first odd one is noted (g_rxOddPort) and logged by the pump.
        if (port != kPort && !g_rxOddPort.load(std::memory_order_relaxed)) g_rxOddPort.store(0x10000u | port, std::memory_order_relaxed);
        if (length <= 0 || length > static_cast<int>(kRawMax) || head - tail == kRxSlots) {
            p.dropped.fetch_add(1, std::memory_order_relaxed); continue;
        }
        {   // (only a datagram that passed the checks above: a stranger's empty ones must not fill the 8-slot table)
            const uint32_t n = g_seenCount.load(std::memory_order_relaxed);
            bool known = false;
            for (uint32_t i = 0; i < n && !known; ++i) known = std::memcmp(g_seenMac[i], from.data, 6) == 0;
            if (!known && n < 8) { std::memcpy(g_seenMac[n], from.data, 6); g_seenCount.store(n + 1, std::memory_order_release); }
        }
        RawRecord &record = p.ring[head % kRxSlots];
        std::memcpy(record.address.bytes, from.data, 6); record.length = static_cast<uint16_t>(length);
        std::memcpy(record.bytes, scratch, static_cast<unsigned>(length));
        p.head.store(head + 1, std::memory_order_release);
        const uint32_t waiting = head + 1 - tail;
        if (waiting > g_ringPeak.load(std::memory_order_relaxed)) g_ringPeak.store(waiting, std::memory_order_relaxed);
    }
    return 0;
}
// One reverse-order release per pump. Never free a live worker's socket/ring.
void cleanup() {
    const uint64_t now = sceKernelGetProcessTimeWide();
    if (now - p.cleanup_wait_us >= 2000000) p.cleanup_stalled = true;
    if (now < p.cleanup_retry_us) return;
    // An in-flight Init/Result can acquire ownership after close was requested.
    // Consume it and end the dialog before releasing its network stack.
    if (mail.state.load(std::memory_order_acquire) != MailState::Idle || p.dialog) {
        p.cleanup_stage = "close_dialog"; cleanup_dialog(); return;
    }
    if (p.worker >= 0) {
        p.cleanup_stage = "close_worker";
        if (p.worker_started) {
            SceUInt timeout = 0;
            int rc = sceKernelWaitThreadEnd(p.worker, nullptr, &timeout);
            if (rc < 0) {
                if (static_cast<uint32_t>(rc) != SCE_KERNEL_ERROR_WAIT_TIMEOUT) cleanup_failed(rc);
                else if (p.cleanup_stalled) p.cleanup_retry_us = now + 1000000;
                return;
            }
            p.worker_started = false;
        }
        if (cleanup_failed(sceKernelDeleteThread(p.worker))) return;
        p.worker = -1; cleanup_progress(); return;
    }
    if (p.ring) { p.cleanup_stage = "close_ring"; ::free(p.ring); p.ring = nullptr; cleanup_progress(); return; }
    if (p.socket >= 0) { p.cleanup_stage = "close_socket"; if (cleanup_soft(sceNetAdhocPdpDelete(p.socket, 0), "sceNetAdhocPdpDelete")) return; p.socket = -1; cleanup_progress(); return; }
    if (p.cell) { p.cleanup_stage = "close_cell"; if (cleanup_soft(sceNetCtlAdhocDisconnect(), "sceNetCtlAdhocDisconnect")) return; p.cell = false; cleanup_progress(); return; }
    if (p.ctl) { p.cleanup_stage = "close_ctl"; if (cleanup_soft(sceNetAdhocctlTerm(), "sceNetAdhocctlTerm")) return; p.ctl = false; cleanup_progress(); return; }
    if (p.adhoc) { p.cleanup_stage = "close_adhoc"; if (cleanup_soft(sceNetAdhocTerm(), "sceNetAdhocTerm")) return; p.adhoc = false; cleanup_progress(); return; }
#if NP_NET_KEEP_STACK
    // Galaxy: sceNet / sceNetCtl (and the NET module) stay up for the life of the process, as the Same Wi-Fi path already does. np_https
    // (g_netUp) and the online socket cache "the stack is up" and share it; a sceNetTerm here, after a Nearby session that was the first to
    // initialise it, left them with a dead stack (resolver / socket errors until the game restarted). The pool now belongs to the stack:
    // it is not freed. The next session's sceNetInit answers EBUSY and frees its own, unused pool (the NET step above).
    if (p.netctl) { p.cleanup_stage = "keep_netctl"; p.netctl = false; cleanup_progress(); return; }
    if (p.net) { p.cleanup_stage = "keep_net"; p.net = false; p.pool = nullptr; NpLogf("vlink-vita", "the sceNet stack stays up after the radio closes (NP_NET_KEEP_STACK)\n"); cleanup_progress(); return; }
#else
    if (p.netctl) { p.cleanup_stage = "close_netctl"; sceNetCtlTerm(); p.netctl = false; cleanup_progress(); return; }
    if (p.net) { p.cleanup_stage = "close_net"; if (cleanup_failed(sceNetTerm())) return; p.net = false; cleanup_progress(); return; }
#endif
    if (p.pool) { p.cleanup_stage = "close_pool"; ::free(p.pool); p.pool = nullptr; cleanup_progress(); return; }
    if (p.module_adhoc) { p.cleanup_stage = "close_module_adhoc"; if (cleanup_soft(sceSysmoduleUnloadModule(SCE_SYSMODULE_PSPNET_ADHOC), "sceSysmoduleUnloadModule(PSPNET_ADHOC)")) return; p.module_adhoc = false; cleanup_progress(); return; }
#if NP_NET_KEEP_STACK
    if (p.module_net) { p.cleanup_stage = "keep_module_net"; p.module_net = false; cleanup_progress(); return; }   // (the stack above still uses it)
#else
    if (p.module_net) { p.cleanup_stage = "close_module_net"; if (cleanup_failed(sceSysmoduleUnloadModule(SCE_SYSMODULE_NET))) return; p.module_net = false; cleanup_progress(); return; }
#endif
    if (!g_skippedThisClose) g_leftovers = nullptr;
    if (g_rxErrCount.load(std::memory_order_relaxed))   // (the worker is gone: the count is final)
        NpLogf("vlink-vita", "radio closed: PdpRecv errors in this session: %u\n", g_rxErrCount.load(std::memory_order_relaxed));
    p.phase = NP_RADIO_DOWN; p.cleanup_stalled = false;
}
}

extern "C" void np_vita_radio_note_leftovers(const char *who) {
    if (g_leftovers) NpLogf("vlink-vita", "%s opens although the ad hoc close gave up on %s earlier: the stack may be half up\n", who ? who : "?", g_leftovers);
}
extern "C" unsigned np_vita_radio_ring_take_peak(void) { return g_ringPeak.exchange(0, std::memory_order_relaxed); }
extern "C" void np_vita_radio_send_take(unsigned* ok, unsigned* blocked, unsigned* err) {   // Galaxy (netlog): the counters restart at each call
    *ok = g_txOk.exchange(0, std::memory_order_relaxed);
    *blocked = g_txBlocked.exchange(0, std::memory_order_relaxed);
    *err = g_txErr.exchange(0, std::memory_order_relaxed);
}
extern "C" void np_vita_radio_send_totals(unsigned* blocked, unsigned* err) {   // Galaxy (netlog): since the radio opened, not reset by a read
    *blocked = g_txBlockedAll.load(std::memory_order_relaxed);
    *err = g_txErrAll.load(std::memory_order_relaxed);
}
extern "C" unsigned np_vita_radio_rx_late_take_max(void) { return g_rxLateMax.exchange(0, std::memory_order_relaxed); }   // Galaxy (MP3 nocap)
extern "C" unsigned np_vita_radio_rx_dropped(void) { return p.dropped.load(std::memory_order_relaxed); }

extern "C" NpRadioStatus np_vita_radio_begin_link() {
    if (p.phase != NP_RADIO_DOWN) return NP_RADIO_WRONG_PHASE;
    g_openUs = 0; g_ringPeak.store(0);
    g_lanGroup = EnvNumber("SMG_LAN_GROUP", 1, 0, 1) ? 1 : 0;
    g_lanTimeoutMs = static_cast<uint32_t>(EnvNumber("SMG_LAN_TIMEOUT_MS", 60000, 0, 3600000));
    {
        const char *mode = std::getenv("SMG_LAN_DIALOG");
        g_lanMode = !mode || !std::strcmp(mode, "conn") ? 0 : !std::strcmp(mode, "create") ? 1 : 2;
        NpLogf("vlink-vita", "Nearby starts: mode %s, group %s, timeout %u ms, link watch %s\n", kModeName[g_lanMode], g_lanGroup ? kGroup : "none",
               g_lanTimeoutMs, LinkWatch() ? "on" : "off");
    }
    g_rxFirstErr.store(0); g_rxErrCount.store(0); g_rxOddPort.store(0); g_rxErrLogged = g_rxPortLogged = false; g_seenCount.store(0); g_seenLogged = 0;
    if (!dialog_ready.load(std::memory_order_acquire)) return NP_RADIO_LIMIT;
    p.step = MODULE_NET; p.error = 0; p.cleanup_error = 0; p.status = NP_RADIO_AGAIN;
    p.head.store(0); p.tail.store(0); p.dropped.store(0); p.rx_error.store(0);
    p.phase = NP_RADIO_STARTING; p.aborted = p.result_read = false;
    p.close_dialog = DialogOp::Status; return NP_RADIO_AGAIN;
}
extern "C" int np_vita_radio_link_send(const uint8_t *mac, const void *bytes, size_t length) {
    if (p.phase != NP_RADIO_OPEN || !bytes || !length) return -1;
    if (length > static_cast<size_t>(SCE_NET_ADHOC_PDP_MFS)) { g_oversizeSends.fetch_add(1, std::memory_order_relaxed); return -1; }
    const auto one = [&](const uint8_t *to) {
        SceNetEtherAddr address{}; std::memcpy(address.data, to, 6);
        const int rc = sceNetAdhocPdpSend(p.socket, &address, kPort, bytes, static_cast<int>(length), 0, SCE_NET_ADHOC_F_NONBLOCK);
        // netlog: what the radio did with the send (np_vita_radio_send_take): taken / "would block" or time-out (treated as sent: NOT retried, the
        // frame is gone; the reliable layer above resends what mattered) / any other error
        if (rc >= 0) g_txOk.fetch_add(1, std::memory_order_relaxed);
        else if (transient(rc)) { g_txBlocked.fetch_add(1, std::memory_order_relaxed); g_txBlockedAll.fetch_add(1, std::memory_order_relaxed); }
        else { g_txErr.fetch_add(1, std::memory_order_relaxed); g_txErrAll.fetch_add(1, std::memory_order_relaxed); }
        return rc >= 0 || transient(rc) ? static_cast<int>(length) : rc;
    };
    if (mac) return one(mac);
    static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    int result = one(broadcast);
    SceNetAdhocctlPeerInfo peers[8]{}; int size = sizeof(peers);
    if (sceNetAdhocctlGetPeerList(&size, peers) >= 0 && size > 0 && size <= static_cast<int>(sizeof(peers)) &&
        size % static_cast<int>(sizeof(peers[0])) == 0) {
        for (int i = 0; i < size / static_cast<int>(sizeof(peers[0])); ++i) {
            const int sent = one(peers[i].macAddr.data);
            if (sent >= 0) result = sent;
        }
    }
    return result;
}
extern "C" int np_vita_radio_link_recv(uint8_t mac[6], void *bytes, size_t capacity) {
    if (p.phase != NP_RADIO_OPEN) return -1;
    const uint32_t tail = p.tail.load(std::memory_order_relaxed);
    if (tail == p.head.load(std::memory_order_acquire)) return 0;
    const RawRecord &record = p.ring[tail % kRxSlots];
    const size_t n = record.length < capacity ? record.length : capacity;
    std::memcpy(mac, record.address.bytes, 6); std::memcpy(bytes, record.bytes, n);
    p.tail.store(tail + 1, std::memory_order_release);
    return static_cast<int>(n);
}
extern "C" bool np_vita_radio_link_mac(uint8_t mac[6]) {
    if (p.phase != NP_RADIO_OPEN) return false;
    std::memcpy(mac, p.mac.data, 6); return true;
}
// vlink_vita.cpp: the Wi-Fi watch and the resume notice, once per presented frame (weak: the recompiled port's copy has none).
extern "C" void np_vita_lan_frame(void) __attribute__((weak));

extern "C" void np_vita_radio_pump() {
    if (np_vita_lan_frame) np_vita_lan_frame();
    if (p.phase == NP_RADIO_DOWN) return;
    if (p.phase == NP_RADIO_CLOSING) { cleanup(); return; }
    if (p.phase == NP_RADIO_OPEN) {
        int state{}; uint32_t error = p.rx_error.load(std::memory_order_acquire);
        if (error) { close_with(NP_RADIO_CLOSED, error); return; }
        if (!g_rxErrLogged && g_rxFirstErr.load(std::memory_order_relaxed)) {
            g_rxErrLogged = true;
            NpLogf("vlink-vita", "PdpRecv error 0x%08X (first of %u so far; the link goes on unless they never stop)\n",
                   g_rxFirstErr.load(std::memory_order_relaxed), g_rxErrCount.load(std::memory_order_relaxed));
        }
        if (!g_rxPortLogged && g_rxOddPort.load(std::memory_order_relaxed)) {
            g_rxPortLogged = true;
            NpLogf("vlink-vita", "a datagram came from PDP port 0x%04X, not %04X: the port byte order differs from what was assumed\n",
                   g_rxOddPort.load(std::memory_order_relaxed) & 0xFFFFu, kPort);
        }
        for (uint32_t n = g_seenCount.load(std::memory_order_acquire); g_seenLogged < n; ++g_seenLogged) {
            const uint8_t *m = g_seenMac[g_seenLogged];
            const char *verdict = "in the peer list";
            bool zero = true; for (int i = 0; i < 6; ++i) zero = zero && m[i] == 0;
            SceNetAdhocctlPeerInfo peers[8]{}; int size = sizeof(peers);
            if (zero) verdict = "ALL ZERO: unicast to it will fail";
            else if (sceNetAdhocctlGetPeerList(&size, peers) < 0 || size < 0 || size > static_cast<int>(sizeof(peers)) || size % static_cast<int>(sizeof(peers[0])) != 0) verdict = "peer list unreadable";   // (the same guard as link_send: a length longer than the buffer is not trusted)
            else {
                bool exact = false, tail5 = false;
                for (int i = 0; i < size / static_cast<int>(sizeof(peers[0])); ++i) {
                    const uint8_t *q = peers[i].macAddr.data;
                    if (!std::memcmp(q, m, 6)) exact = true;
                    else if (!std::memcmp(q + 1, m + 1, 5)) tail5 = true;
                }
                verdict = exact ? "in the peer list" : tail5 ? "FIRST BYTE differs from the peer list's: unicast to it may fail" : "NOT in the peer list (yet?)";
            }
            NpLogf("vlink-vita", "first datagram from %02X:%02X:%02X:%02X:%02X:%02X: %s\n", m[0], m[1], m[2], m[3], m[4], m[5], verdict);
        }
        if (!LinkWatch()) {
            if (sceNetCtlAdhocGetState(&state) >= 0 && state == SCE_NETCTL_STATE_DISCONNECTED) close_with(NP_RADIO_CLOSED, error);
            return;  // the LAN link polls the receive ring itself
        }
        const uint64_t now = sceKernelGetProcessTimeWide();
        if (!g_openUs) { g_openUs = now; g_nextLookUs = now + 1000000; g_cellStrikes = 0; g_peerSeen = g_noPeerNoted = false; }
        if (now < g_nextLookUs) return;  // once a second
        g_nextLookUs = now + 1000000;
        if (sceNetCtlAdhocGetState(&state) >= 0 && state == SCE_NETCTL_STATE_DISCONNECTED) {
            if (++g_cellStrikes >= 2) {
                NpLogf("vlink-vita", "ad hoc cell gone (two looks a second apart): closing the radio\n");
                close_with(NP_RADIO_CLOSED, error);
            }
            return;
        }
        g_cellStrikes = 0;
        if (!g_peerSeen) {
            SceNetAdhocctlPeerInfo peers[8]{}; int size = sizeof(peers);
            if (sceNetAdhocctlGetPeerList(&size, peers) >= 0 && size > 0) {
                g_peerSeen = true;
                NpLogf("vlink-vita", "a nearby console is in the cell (%.1f s after it opened)\n", double(now - g_openUs) / 1e6);
            } else if (!g_noPeerNoted && now - g_openUs > 30000000) {
                g_noPeerNoted = true;
                NpLogf("vlink-vita", "no other console in the ad hoc cell after 30 s: if the two Vitas do not find each other, set "
                                     "MULTIPLAYER LINK to NEARBY HOST on one and NEARBY JOIN on the other (SMG_LAN_DIALOG=create / join)\n");
            }
        }
        return;  // the LAN link polls the receive ring itself
    }
    int rc{};
    switch (p.step) {
    case MODULE_NET:
    case MODULE_ADHOC: {
        const auto id = p.step == MODULE_NET ? SCE_SYSMODULE_NET : SCE_SYSMODULE_PSPNET_ADHOC;
        if (sceSysmoduleIsLoaded(id) != SCE_SYSMODULE_LOADED) {
            if (failed(sceSysmoduleLoadModule(id))) return;
            (p.step == MODULE_NET ? p.module_net : p.module_adhoc) = true;
        }
        break;
    }
    case POOL:
        p.pool = ::calloc(1, kNetPool); if (!p.pool) { close_with(NP_RADIO_LIMIT); return; } break;
    case NET: {
        SceNetInitParam param{}; param.memory = p.pool; param.size = kNetPool;
        rc = sceNetInit(&param);
        NpLogf("vlink-vita", "sceNetInit (ad hoc, pool %u KiB): 0x%08X\n", kNetPool / 1024u, static_cast<unsigned>(rc));
        if (static_cast<uint32_t>(rc) == SCE_NET_ERROR_EBUSY) { ::free(p.pool); p.pool = nullptr; }
        else { if (failed(rc)) return; p.net = true; }
        break;
    }
    case NETCTL:
        rc = sceNetCtlInit();
        if (static_cast<uint32_t>(rc) != 0x80412102u) { if (failed(rc)) return; p.netctl = true; }
        break;
    case ADHOC:
        rc = sceNetAdhocInit();
        // Galaxy (R5): a sceNetAdhocTerm that was skipped after three failures leaves the library initialised: not an error for the next session.
        if (static_cast<uint32_t>(rc) == SCE_ERROR_NET_ADHOC_ALREADY_INITIALIZED) { NpLogf("vlink-vita", "sceNetAdhocInit: already initialised (an earlier close skipped its Term): using it\n"); rc = 0; }
        if (failed(rc)) return;
        p.adhoc = true; break;
    case CTL: {
        SceNetAdhocctlAdhocId id{}; id.type = SCE_NET_ADHOCCTL_ADHOCTYPE_RESERVED;
        std::memcpy(id.data, kAdhocId, 9);
        g_ctlId = kAdhocId; g_ctlFirstError = 0;
        // ALREADY_INITIALIZED = an earlier close skipped its Term (see ADHOC): the library keeps the id it was initialised with (g_ctlInitId: recorded
        // only when an Init was accepted; it may be the title-id fallback), and the dialog must use that one.
        const auto adopt = [&]() {
            g_ctlId = g_ctlInitId ? g_ctlInitId : kAdhocId;
            NpLogf("vlink-vita", "ad hoc id %s: adopted (sceNetAdhocctlInit says already initialised; an earlier close skipped its Term)\n", g_ctlId);
            p.ctl = true;
        };
        rc = sceNetAdhocctlInit(&id);
        if (static_cast<uint32_t>(rc) == SCE_ERROR_NET_ADHOCCTL_ALREADY_INITIALIZED) { adopt(); break; }
        if (rc < 0 && TitleId() && std::strcmp(TitleId(), kAdhocId) != 0) {
            // Fallback (wave 5 NET): the running title id, as the shipping references pass.
            g_ctlFirstError = static_cast<uint32_t>(rc);
            std::memcpy(id.data, TitleId(), 9);
            g_ctlId = TitleId();
            rc = sceNetAdhocctlInit(&id);
            if (static_cast<uint32_t>(rc) == SCE_ERROR_NET_ADHOCCTL_ALREADY_INITIALIZED) { adopt(); break; }
        }
        if (rc >= 0) g_ctlInitId = g_ctlId;
        NpLogf("vlink-vita", "ad hoc id %s: %s (0x%08X)%s\n", g_ctlId, rc >= 0 ? "accepted" : "refused", static_cast<uint32_t>(rc),
                     g_ctlFirstError ? " after SMGV00001 was refused" : "");
        if (failed(rc)) return;
        p.ctl = true; break;
    }
    case DIALOG_INIT: {
        if (!dialog_ready.load(std::memory_order_acquire)) { close_with(NP_RADIO_LIMIT); return; }
        DialogReply reply{}; if (!call_dialog(DialogOp::Init, reply)) return;
        if (!g_idLogged) {
            g_idLogged = true;
            static const char *const kVariant[] = {"the ad hoc id with group SMGMP001", "the ad hoc id without a group name", "the title id without a group name"};
            NpLogf("vlink-vita", "NetCheck dialog (%s): %s with %s%s (first refusal 0x%08X)\n", kModeName[g_lanMode],
                         reply.rc >= 0 ? "started" : "refused", kVariant[g_dialogVariant], g_dialogVariant > (g_lanGroup ? 0 : 1) ? ", a fallback" : "", g_dialogFirstError);
        }
        if (failed(reply.rc)) return;
        p.phase = NP_RADIO_DIALOG; break;
    }
    case DIALOG_POLL: {
        DialogReply reply{}; if (!call_dialog(DialogOp::Status, reply)) return;
        if (failed(reply.rc)) return;
        if (reply.rc == SCE_COMMON_DIALOG_STATUS_RUNNING) return;
        if (reply.rc != SCE_COMMON_DIALOG_STATUS_FINISHED) { close_with(NP_RADIO_CLOSED); return; }
        break;
    }
    case DIALOG_RESULT: {
        DialogReply reply{}; if (!call_dialog(DialogOp::Result, reply)) return;
        if (failed(reply.rc)) return;
        NpLogf("vlink-vita", "NetCheck dialog result 0x%08X (%s)\n", static_cast<unsigned>(reply.result),
               reply.result == SCE_COMMON_DIALOG_RESULT_OK ? "ok, in a cell" : reply.result == SCE_COMMON_DIALOG_RESULT_USER_CANCELED ? "cancelled by the player" : "not connected");
        if (reply.result != SCE_COMMON_DIALOG_RESULT_OK) { close_with(NP_RADIO_CLOSED, reply.result); return; }
        break;
    }
    case DIALOG_TERM: {
        DialogReply reply{}; if (!call_dialog(DialogOp::Term, reply)) return;
        if (failed(reply.rc)) return;
        p.phase = NP_RADIO_STARTING; break;
    }
    case MAC:
        rc = sceNetAdhocctlGetEtherAddr(&p.mac);
        NpLogf("vlink-vita", "sceNetAdhocctlGetEtherAddr: 0x%08X (%02X:%02X:%02X:%02X:%02X:%02X)\n", static_cast<unsigned>(rc), p.mac.data[0], p.mac.data[1], p.mac.data[2], p.mac.data[3], p.mac.data[4], p.mac.data[5]);
        if (failed(rc)) return;
        if (!(p.mac.data[0] | p.mac.data[1] | p.mac.data[2] | p.mac.data[3] | p.mac.data[4] | p.mac.data[5]))
            NpLogf("vlink-vita", "WARNING: our own ad hoc MAC is all zero\n");
        break;
    case SOCKET:
        rc = sceNetAdhocPdpCreate(&p.mac, kPort, 65536, 0);
        NpLogf("vlink-vita", "sceNetAdhocPdpCreate port %u: %d (0x%08X)\n", kPort, rc, static_cast<unsigned>(rc));
        if (failed(rc)) return;
        p.socket = rc; break;
    case RING:
        p.ring = static_cast<RawRecord *>(::calloc(kRxSlots, sizeof(RawRecord)));
        if (!p.ring) { close_with(NP_RADIO_LIMIT); return; } break;
    case THREAD_CREATE:
        // Core 2 (SCE_KERNEL_CPU_MASK_USER_2): the platform plan keeps the game thread on core 0 and the renderer on core 1; this thread
        // only waits for datagrams (a 10 ms receive time-out) and must not take time from either.
        // Galaxy (MP3 nocap): SMG_MP_RX_PRIO=0x40..0xBF in env.txt (default: the process default priority, as in Strikers) for a console
        // A/B when the "[mp] net" line's "rx late max" shows this thread waiting behind the front end's player on core 2.
        rc = sceKernelCreateThread("smg_mp_rx", receive_worker, rx_priority(), 65536, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
        if (failed(rc)) return;
        p.worker = rc; break;
    case THREAD_START:
        p.run.store(true, std::memory_order_release);
        if (failed(sceKernelStartThread(p.worker, 0, nullptr))) return;
        p.worker_started = true; break;
    case OPENING: p.phase = NP_RADIO_OPEN; p.status = NP_RADIO_OK; break;
    case ACTIVE: return;
    }
    p.step = static_cast<Step>(p.step + 1);
}
extern "C" void np_vita_radio_close() { close_with(NP_RADIO_CLOSED); }
extern "C" void np_vita_radio_suspend() { close_with(NP_RADIO_CLOSED); }
extern "C" void np_vita_radio_snapshot(NpRadioSnapshot *out) {
    if (out) *out = {p.phase, p.status, p.phase == NP_RADIO_CLOSING ? p.cleanup_stage : names[p.step],
                    p.error ? p.error : p.cleanup_error, p.dropped.load(std::memory_order_relaxed), p.cleanup_stalled,
                    g_oversizeSends.load(std::memory_order_relaxed)};
}
extern "C" void np_vita_radio_set_dialog_ready(bool ready) { dialog_ready.store(ready, std::memory_order_release); }
// W5 BOOT: the system set-up the NetCheck dialog needs (AppUtil, then the CommonDialog configuration). Every console log since 17 Oct 00:50 had
// "CommonDialog configuration failed" and nothing said which call refused or why; with dialog_ready false np_vita_radio_begin_link refuses
// (NP_RADIO_LIMIT), i.e. NEARBY (ad hoc) play could not start at all. Now: every result is logged, the set-up is first tried on the
// process's main thread at start-up (np_vita_radio_system_init, from np_vita_main.cpp: where the SDK samples and VitaShell do it), with the
// language and enter button the system has (as they do; the sentinels of sceCommonDialogConfigParamInit are the fallback), and the renderer's
// call only completes what is missing.
namespace {
int g_cfgAppUtilRc = 0x7FFFFFFF, g_cfgParamRc = 0x7FFFFFFF, g_cfgLang = -1, g_cfgEnter = -1;
bool configure_dialogs(const char *where) {
    if (!renderer_apputil) {
        SceAppUtilInitParam init{}; SceAppUtilBootParam boot{};
        g_cfgAppUtilRc = sceAppUtilInit(&init, &boot);
        if (g_cfgAppUtilRc >= 0 || static_cast<uint32_t>(g_cfgAppUtilRc) == SCE_APPUTIL_ERROR_BUSY) renderer_apputil = true;  // BUSY: already initialised
    }
    if (!renderer_config) {
        SceCommonDialogConfigParam config{}; sceCommonDialogConfigParamInit(&config);
        int lang = -1, enter = -1;
        if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang) >= 0) config.language = static_cast<SceSystemParamLang>(lang);
        if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter) >= 0) config.enterButtonAssign = static_cast<SceSystemParamEnterButtonAssign>(enter);
        g_cfgLang = lang; g_cfgEnter = enter;
        g_cfgParamRc = sceCommonDialogSetConfigParam(&config);
        if (g_cfgParamRc < 0 && (lang >= 0 || enter >= 0)) {   // the sentinels ("the system's own") as the second try
            sceCommonDialogConfigParamInit(&config);
            const int second = sceCommonDialogSetConfigParam(&config);
            if (second >= 0) g_cfgParamRc = second;
        }
        if (g_cfgParamRc >= 0) renderer_config = true;
    }
    SceKernelThreadInfo info{}; info.size = sizeof info;
    sceKernelGetThreadInfo(sceKernelGetThreadId(), &info);
    NpLogf("vlink-vita", "CommonDialog set-up on the %s thread ('%s'): sceAppUtilInit 0x%08X, system language %d, enter button %d, "
                  "sceCommonDialogSetConfigParam 0x%08X: %s\n", where, info.name, static_cast<unsigned>(g_cfgAppUtilRc), g_cfgLang, g_cfgEnter,
                  static_cast<unsigned>(g_cfgParamRc), renderer_apputil && renderer_config ? "ready (NEARBY play can open the system's ad hoc dialog)"
                                                                                           : "NOT ready (NEARBY play cannot start; SAME WI-FI works)");
    return renderer_apputil && renderer_config;
}
} // namespace
extern "C" int np_vita_radio_system_init(void) { return configure_dialogs("main") ? 0 : -1; }
extern "C" int np_vita_radio_dialog_prepare(bool surface_ready) {
    dialog_ready.store(false, std::memory_order_release);
    if (!surface_ready) return NP_RADIO_LIMIT;
    if (!(renderer_apputil && renderer_config) && !configure_dialogs("renderer")) return NP_RADIO_CLOSED;
    dialog_ready.store(true, std::memory_order_release); return NP_RADIO_OK;
}
extern "C" void np_vita_ime_dialog_render(void) __attribute__((weak));   // platform/sdk/dwc/np_online_ime_vita.cpp (the name keyboard)
extern "C" bool np_vita_ime_dialog_active(void) __attribute__((weak));
extern "C" bool np_vita_radio_dialogs_ready() { return dialog_ready.load(std::memory_order_acquire); }
extern "C" void np_vita_radio_dialog_render() {
    if (np_vita_ime_dialog_render) np_vita_ime_dialog_render();
    if (mail.state.load(std::memory_order_acquire) != MailState::Requested) return;
    auto &reply = mail.reply;
    switch (reply.op) {
    case DialogOp::Init: {
        // Both consoles connect to the same group; the game's own lobby then decides who hosts. Fallbacks (wave 5 NET) only when the
        // dialog refuses to start: no group name, then the running title id without a group (the shipping references' parameters).
        // The id follows sceNetAdhocctlInit's choice.
        const int dialogMode = g_lanMode == 0 ? SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_CONN :
            g_lanMode == 1 ? SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_CREATE : SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_JOIN;
        g_dialogFirstError = 0;
        int tried = g_lanGroup ? 0 : 1;
        for (int variant = g_lanGroup ? 0 : 1; variant < 3; ++variant) {
            const char *id = variant == 2 ? TitleId() : g_ctlId;
            if (!id || (variant == 2 && !std::strcmp(id, g_ctlId))) continue;
            tried = variant;
            sceNetCheckDialogParamInit(&p.dialog_param);
            std::memset(&p.group, 0, sizeof p.group);
            std::memcpy(p.group.data, kGroup, 8);
            p.dialog_param.groupName = variant == 0 ? &p.group : nullptr;
            std::memcpy(p.dialog_param.npCommunicationId.data, id, 9);
            p.dialog_param.mode = static_cast<decltype(p.dialog_param.mode)>(dialogMode);
            p.dialog_param.timeoutUs = g_lanTimeoutMs * 1000u;   // SMG_LAN_TIMEOUT_MS (default 60000)
            reply.rc = sceNetCheckDialogInit(&p.dialog_param);
            if (reply.rc >= 0) break;
            if (!g_dialogFirstError) g_dialogFirstError = static_cast<uint32_t>(reply.rc);
        }
        g_dialogVariant = tried;
        g_idLogged = false;
        if (reply.rc >= 0) dialog_visible.store(true, std::memory_order_release);
        break;
    }
    case DialogOp::Status: reply.rc = sceNetCheckDialogGetStatus(); break;
    case DialogOp::Result: {
        SceNetCheckDialogResult result{}; reply.rc = sceNetCheckDialogGetResult(&result); reply.result = result.result; break;
    }
    case DialogOp::Abort: reply.rc = sceNetCheckDialogAbort(); break;
    case DialogOp::Term:
        reply.rc = sceNetCheckDialogTerm();
        if (reply.rc >= 0) dialog_visible.store(false, std::memory_order_release);
        break;
    }
    mail.state.store(MailState::Completed, std::memory_order_release);
}
extern "C" bool np_vita_radio_dialog_active() {
    return dialog_visible.load(std::memory_order_acquire) || (np_vita_ime_dialog_active && np_vita_ime_dialog_active());
}

#endif  // NP_TARGET_VITA
