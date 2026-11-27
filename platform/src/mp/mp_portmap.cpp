// Galaxy 3.0 multiplayer (port side, owner: netx): the router port mapping of an internet host. See mp_portmap.h.
#include "mp_portmap.h"

#include "mp_config.h"
#include "mp_core_sys.h"
#include "port/mp_bridge.h"   // mp_log

#include "np_natpmp.h"
#include "np_upnp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

MpMutex& Mu() {
    static MpMutex m;
    return m;
}

struct Request {
    uint8_t owners;   // bit per MpPortmapOwner: who wants the mapping (0 = nobody)
    uint16_t port;
    uint32_t ip;
    char who[16];
};
Request g_req;               // under Mu()
MpPortmapInfo g_info;        // under Mu()

// pump side (only mp_portmap_pump / mp_portmap_finish touch these; they never run at the same time: the helper thread is joined first)
NpNet::Upnp* g_upnp = 0;
NpNet::NatPmp* g_pmp = 0;    // MP3 natpmp: NAT-PMP / PCP, tried in parallel with UPnP (null when SMG_MP_NATPMP=0)
int g_winner = 0;            // 0 = nobody yet, 1 = UPnP, 2 = NAT-PMP / PCP: the first to map wins, the other is cancelled and unmapped
Request g_run;               // what g_upnp was started for
bool g_removing = false;
volatile bool g_pumpBusy = false;

bool Enabled() {
    const char* v = getenv("SMG_MP_UPNP");
    return !(v && v[0] == '0');
}

bool PmpEnabled() {
    const char* v = getenv("SMG_MP_NATPMP");
    return !(v && v[0] == '0');
}

void LogLine(void*, const char* line) { mp_log("upnp: %s", line); }
void PmpLogLine(void*, const char* line) { mp_log("natpmp: %s", line); }

uint8_t StateOf(const NpNet::Upnp& u) {
    switch (u.GetState()) {
    case NpNet::Upnp::MAPPED:
    case NpNet::Upnp::RENEWING: return MP_PORTMAP_MAPPED;
    case NpNet::Upnp::FAILED: return MP_PORTMAP_FAILED;
    case NpNet::Upnp::IDLE:
    case NpNet::Upnp::REMOVED: return MP_PORTMAP_OFF;
    default: return MP_PORTMAP_WORKING;
    }
}

uint8_t StateOf(const NpNet::NatPmp& p) {
    switch (p.GetState()) {
    case NpNet::NatPmp::MAPPED:
    case NpNet::NatPmp::RENEWING: return MP_PORTMAP_MAPPED;
    case NpNet::NatPmp::FAILED: return MP_PORTMAP_FAILED;
    case NpNet::NatPmp::IDLE:
    case NpNet::NatPmp::REMOVED: return MP_PORTMAP_OFF;
    default: return MP_PORTMAP_WORKING;
    }
}

// What the player-facing state is: the winner's; before there is one, WORKING while either protocol still tries, FAILED when all failed.
// The addresses a failed protocol still learned (the router's public address) are kept, so the lobby can say "your provider blocks this".
void Publish(bool removing) {
    MpPortmapInfo n;
    memset(&n, 0, sizeof n);
    if (!removing && (g_upnp || g_pmp)) {
        const uint8_t su = g_upnp ? StateOf(*g_upnp) : uint8_t(MP_PORTMAP_OFF);
        const uint8_t sp = g_pmp ? StateOf(*g_pmp) : uint8_t(MP_PORTMAP_OFF);
        n.intPort = g_run.port;
        if (g_winner == 1 && g_upnp) {
            n.state = su;
            n.extIp = g_upnp->ExternalIp();
            n.extPort = g_upnp->ExternalPort();
            n.cgnat = g_upnp->ExternalIsPrivate();
            if (su == MP_PORTMAP_FAILED) snprintf(n.error, sizeof n.error, "%s", g_upnp->Error());
        } else if (g_winner == 2 && g_pmp) {
            n.state = sp;
            n.extIp = g_pmp->ExternalIp();
            n.extPort = g_pmp->ExternalPort();
            n.cgnat = g_pmp->ExternalIsPrivate();
            if (sp == MP_PORTMAP_FAILED) snprintf(n.error, sizeof n.error, "%s", g_pmp->Error());
        } else {
            const bool uDone = !g_upnp || su == MP_PORTMAP_FAILED, pDone = !g_pmp || sp == MP_PORTMAP_FAILED;
            n.state = (uDone && pDone) ? MP_PORTMAP_FAILED : MP_PORTMAP_WORKING;
            const uint32_t ui = g_upnp ? g_upnp->ExternalIp() : 0, pi = g_pmp ? g_pmp->ExternalIp() : 0;
            n.extIp = ui ? ui : pi;
            n.cgnat = (g_upnp && g_upnp->ExternalIsPrivate()) || (g_pmp && g_pmp->ExternalIsPrivate());
            if (n.state == MP_PORTMAP_FAILED) {
                // UPnP's sentence, unless only the NAT-PMP / PCP side found a router (then its reason is the useful one)
                const bool pmpMoreUseful = g_pmp && g_pmp->Answered() && !ui;
                snprintf(n.error, sizeof n.error, "%s", pmpMoreUseful || !g_upnp ? g_pmp->Error() : g_upnp->Error());
            }
        }
    }
    MpLock l(Mu());
    n.serial = g_info.serial;
    if (memcmp(&n, &g_info, sizeof n) != 0) {
        n.serial = g_info.serial + 1;
        g_info = n;
    }
}

void Start(const Request& r, uint64_t now) {
    NpNet::UpnpConfig c;
    c.internalPort = r.port;
    c.internalIp = r.ip;
    c.description = "Super Mario Galaxy";
    c.log = LogLine;
    if (const char* t = getenv("SMG_MP_UPNP_SSDP")) {
        NpNet::Addr a;
        if (NpNet::ParseAddr(t, 1900, &a)) c.ssdpTarget = a;
    }
    if (const char* l = getenv("SMG_MP_UPNP_LEASE")) {
        const long v = atol(l);
        if (v >= 0 && v <= 604800) c.leaseSec = uint32_t(v);
    }
    c.staleExternalPort = mp_config()->upnpPermanent;
    g_upnp = new NpNet::Upnp;
    g_run = r;
    g_removing = false;
    g_winner = 0;
    mp_log("upnp: asking the router to forward UDP %u (%s)", unsigned(r.port), r.who);
    g_upnp->Start(c, now);   // (a failure shows as FAILED with its reason, logged once)
    // MP3 natpmp: NAT-PMP / PCP in parallel (routers that do not speak UPnP: AirPort, natpmpd, some ISP boxes)
    if (PmpEnabled()) {
        NpNet::NatPmpConfig pc;
        pc.internalPort = r.port;
        pc.internalIp = r.ip;
        pc.log = PmpLogLine;
        if (const char* t = getenv("SMG_MP_NATPMP_GW")) {   // tests: a fake gateway
            NpNet::Addr a;
            if (NpNet::ParseAddr(t, 0, &a)) pc.gateway = a;
        }
        if (const char* l = getenv("SMG_MP_NATPMP_LEASE")) {
            const long v = atol(l);
            if (v > 0 && v <= 604800) pc.leaseSec = uint32_t(v);
        }
        g_pmp = new NpNet::NatPmp;
        mp_log("natpmp: asking the router to forward UDP %u (%s)", unsigned(r.port), r.who);
        g_pmp->Start(pc, now);
    }
}

// corefix 8: a router that only takes a mapping with no lease keeps it for ever: the port is recorded in mp.cfg (upnp_permanent) while it
// is mapped, so that the next start deletes it (UpnpConfig::staleExternalPort) if the game was killed before it could.
void Record(uint16_t port) {
    if (mp_config()->upnpPermanent == port) return;
    mp_config_record_upnp_permanent(port);
    if (port) mp_log("upnp: the mapping has no lease: recorded in mp.cfg (deleted at exit and at the next start)");
}

void Drop() {
    delete g_upnp;
    g_upnp = 0;
    delete g_pmp;
    g_pmp = 0;
    g_winner = 0;
    g_removing = false;
    memset(&g_run, 0, sizeof g_run);
}

}  // namespace

extern "C" {

bool mp_portmap_request_as(uint8_t owner, uint16_t internalPort, uint32_t localIp, const char* who) {
    if (!internalPort || owner > MP_PORTMAP_TEST) return false;
    static bool loggedOff = false;
    if (!Enabled()) {
        if (!loggedOff) mp_log("upnp: off (SMG_MP_UPNP=0)");
        loggedOff = true;
        return false;
    }
    const uint8_t bit = uint8_t(1u << owner);
    MpLock l(Mu());
    // (a test does not care which local address: 0 matches the room's)
    if (g_req.owners != 0 && g_req.port == internalPort && (g_req.ip == localIp || (owner == MP_PORTMAP_TEST && localIp == 0))) {   // the running mapping: one more owner of it
        g_req.owners |= bit;
        return true;
    }
    if (g_req.owners != 0 && owner == MP_PORTMAP_TEST && (g_req.owners & ~bit)) return false;   // a session's mapping is never replaced by a test
    g_req.owners = bit;   // (a new port: whoever held the old one lost it; only the same owner or a test can have)
    g_req.port = internalPort;
    g_req.ip = localIp;
    snprintf(g_req.who, sizeof g_req.who, "%s", who ? who : "");
    g_pumpBusy = true;
    // working from now on (the helper thread starts the search at its next pump)
    memset(g_info.error, 0, sizeof g_info.error);
    g_info.state = MP_PORTMAP_WORKING;
    g_info.intPort = internalPort;
    g_info.extPort = 0;
    g_info.cgnat = false;
    g_info.serial++;
    return true;
}

void mp_portmap_request(uint16_t internalPort, uint32_t localIp, const char* who) {
    mp_portmap_request_as(MP_PORTMAP_SESSION, internalPort, localIp, who);
}

void mp_portmap_release_as(uint8_t owner) {
    if (owner > MP_PORTMAP_TEST) return;
    MpLock l(Mu());
    g_req.owners &= uint8_t(~(1u << owner));
}

void mp_portmap_release(void) { mp_portmap_release_as(MP_PORTMAP_SESSION); }

void mp_portmap_info(MpPortmapInfo* out) {
    if (!out) return;
    MpLock l(Mu());
    *out = g_info;
    if (g_req.owners == 0 && out->state != MP_PORTMAP_OFF) out->state = MP_PORTMAP_OFF;   // (released: whatever the remover still does)
}

void mp_portmap_pump(void) {
    Request r;
    {
        MpLock l(Mu());
        r = g_req;
    }
    const uint64_t now = mp_sys_now_ms();
    if (g_upnp) {
        const bool same = r.owners != 0 && r.port == g_run.port && r.ip == g_run.ip;
        if (!same && !g_removing) {
            g_removing = true;
            g_upnp->Remove(now);
            if (g_pmp) g_pmp->Remove(now);
        }
        g_upnp->Step(now);
        if (g_pmp) g_pmp->Step(now);
        if (g_upnp->GetState() == NpNet::Upnp::MAPPED) Record(g_upnp->Permanent() ? g_upnp->ExternalPort() : 0);
        if (g_removing) {
            if (g_upnp->Finished() && (!g_pmp || g_pmp->Finished())) {
                if (g_upnp->GetState() == NpNet::Upnp::REMOVED && g_upnp->RemovedOk()) Record(0);   // (a delete that failed keeps the record)
                Drop();
                Publish(true);
            }
        } else {
            // MP3 natpmp: the first protocol to hold a mapping wins; the other is cancelled and, if it got as far as making one, unmapped
            if (!g_winner) {
                if (StateOf(*g_upnp) == MP_PORTMAP_MAPPED) {
                    g_winner = 1;
                    if (g_pmp) {
                        g_pmp->Remove(now);
                        mp_log("natpmp: cancelled (UPnP mapped first)");
                    }
                } else if (g_pmp && StateOf(*g_pmp) == MP_PORTMAP_MAPPED) {
                    g_winner = 2;
                    g_upnp->Remove(now);
                    mp_log("upnp: cancelled (NAT-PMP/PCP mapped first)");
                }
            }
            Publish(false);
        }
    }
    if (!g_upnp && r.owners != 0) {
        Start(r, now);
        Publish(false);
    }
    g_pumpBusy = g_upnp != 0 || r.owners != 0;
}

bool mp_portmap_busy(void) { return g_pumpBusy; }

void mp_portmap_finish(unsigned maxMs) {
    {
        MpLock l(Mu());
        g_req.owners = 0;   // shutdown: every owner's claim ends
    }
    const uint64_t until = mp_sys_now_ms() + maxMs;
    while (g_upnp && mp_sys_now_ms() < until) {
        mp_portmap_pump();
        if (g_upnp) mp_sys_sleep_ms(5);
    }
    if (g_upnp) {
        g_upnp->Abort();   // (it runs out with its lease)
        if (g_pmp) g_pmp->Abort();
        Drop();
    }
    g_pumpBusy = false;
}

}  // extern "C"
