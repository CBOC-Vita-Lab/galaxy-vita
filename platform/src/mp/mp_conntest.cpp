// Galaxy 3.0 multiplayer (owner: conntest): the Connection Test engine. See mp_conntest.h.
#include "mp_conntest.h"

#include <string.h>

// ---- pure helpers -----------------------------------------------------------------------------------------------------------------
bool mp_ct_ip_not_public(uint32_t ip) {
    const uint32_t a = ip >> 24, b = (ip >> 16) & 255;
    if (a == 0 || a == 10 || a == 127) return true;
    if (a == 100 && b >= 64 && b <= 127) return true;      // 100.64.0.0/10: carrier-grade NAT
    if (a == 169 && b == 254) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    return false;
}

uint8_t mp_ct_wifi_result(const MpCtWifi& w) {
    if (!w.connected) return MPCT_R_WIFI_NONE;
    if (w.quality < 0 || w.quality > 100) return MPCT_R_WIFI_UNKNOWN;   // (a number outside 0..100 is not a measurement)
    return w.quality < MP_CT_WEAK_PERCENT ? MPCT_R_WIFI_WEAK : MPCT_R_WIFI_GOOD;
}

uint8_t mp_ct_delay_result(int ms) {
    if (ms < 0 || ms > 600000) return MPCT_R_NONE;
    if (ms < MP_CT_FAST_MS) return MPCT_R_DELAY_FAST;
    return ms < MP_CT_SLOW_MS ? MPCT_R_DELAY_OK : MPCT_R_DELAY_SLOW;
}

uint8_t mp_ct_host_result(bool mapAsked, const MpCtMap& map, bool stunAsked, int stunState, uint32_t stunIp) {
    const bool mapped = mapAsked && map.state == MPCT_MAP_MAPPED;
    const bool routerPrivate = mapAsked && (map.cgnat || (map.extIp != 0 && mp_ct_ip_not_public(map.extIp)));
    const bool routerPublic = mapAsked && map.extIp != 0 && !mp_ct_ip_not_public(map.extIp);
    const bool stunSeen = stunAsked && stunState == 1 && stunIp != 0;
    const bool stunPrivate = stunSeen && mp_ct_ip_not_public(stunIp);
    const bool stunPublic = stunSeen && !stunPrivate;
    // another NAT in front of the router: the router's own "public" address is private, or the internet sees another address than it has
    if (routerPrivate || stunPrivate) return MPCT_R_HOST_CGNAT;
    if (stunPublic && routerPublic && stunIp != map.extIp) return MPCT_R_HOST_CGNAT;
    if (mapped) return MPCT_R_HOST_OPEN;
    if (stunPublic || routerPublic) return MPCT_R_HOST_MAYBE;
    return MPCT_R_HOST_UNKNOWN;
}

bool mp_ct_version_older(const char* mine, const char* minimum) {
    auto parse = [](const char* s, uint32_t out[4]) {
        int n = 0;
        if (!s || !*s) return 0;
        for (;;) {
            if (*s < '0' || *s > '9' || n >= 4) return 0;
            uint32_t v = 0;
            int digits = 0;
            while (*s >= '0' && *s <= '9') {
                if (++digits > 9) return 0;
                v = v * 10 + uint32_t(*s - '0');
                ++s;
            }
            out[n++] = v;
            if (!*s) return n;
            if (*s != '.') return 0;
            ++s;
        }
    };
    uint32_t a[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0};
    if (!parse(mine, a) || !parse(minimum, b)) return false;
    for (int i = 0; i < 4; i++)
        if (a[i] != b[i]) return a[i] < b[i];
    return false;
}

MpCtVerdict mp_ct_verdict(const uint8_t r[MPCT_CHECKS]) {
    MpCtVerdict v;
    memset(&v, 0, sizeof v);
    const uint8_t w = r[MPCT_WIFI], l = r[MPCT_LOCAL], n = r[MPCT_INTERNET], h = r[MPCT_HOSTING], d = r[MPCT_DELAY];
    auto add = [&v](MpCtLine line) {
        if (v.lineCount >= MPCT_MAX_LINES) return;
        for (int i = 0; i < v.lineCount; i++)
            if (v.lines[i] == line) return;
        v.lines[v.lineCount++] = line;
    };
    const bool blocked = l == MPCT_R_LOCAL_BLOCKED;
    v.weakLink = w == MPCT_R_WIFI_WEAK || d == MPCT_R_DELAY_SLOW;
    const bool hostMaybe = h == MPCT_R_HOST_MAYBE || h == MPCT_R_HOST_UNKNOWN, hostCgnat = h == MPCT_R_HOST_CGNAT;
    if (w == MPCT_R_WIFI_NONE) {
        v.headline = MPCT_H_NO_WIFI;
        add(MPCT_L_CONNECT_WIFI), add(MPCT_L_NEARBY_STILL);
    } else if (w == MPCT_R_WIFI_WEAK) {
        v.headline = MPCT_H_WIFI_WEAK;
        add(MPCT_L_MOVE_CLOSER);
        if (n == MPCT_R_NET_OK && hostCgnat) add(MPCT_L_HOST_CGNAT);
        else if (n == MPCT_R_NET_OK && hostMaybe) add(MPCT_L_HOST_MAYBE);
        if (n != MPCT_R_NET_OK) add(MPCT_L_LOCAL_STILL);
        else if (hostCgnat || hostMaybe) add(MPCT_L_LET_FRIEND_HOST);
    } else if (n == MPCT_R_NET_NOT_SET_UP) {
        v.headline = MPCT_H_NOT_SET_UP;
        add(MPCT_L_NOT_SET_UP), add(MPCT_L_LOCAL_STILL);
    } else if (n == MPCT_R_NET_UPDATE) {
        v.headline = MPCT_H_UPDATE;
        add(MPCT_L_UPDATE), add(MPCT_L_LOCAL_STILL);
    } else if (n == MPCT_R_NET_OFF) {
        v.headline = MPCT_H_PAUSED;
        add(MPCT_L_TRY_LATER), add(MPCT_L_LOCAL_STILL);
    } else if (n != MPCT_R_NET_OK) {   // down, skipped, or never finished
        v.headline = MPCT_H_NO_INTERNET;
        add(MPCT_L_CHECK_WIFI);
        add(blocked ? MPCT_L_NEARBY_STILL : MPCT_L_LOCAL_STILL);
    } else if (d == MPCT_R_DELAY_SLOW) {
        v.headline = MPCT_H_SLOW;
        add(MPCT_L_SLOW_LAG);
        if (hostCgnat || hostMaybe) add(MPCT_L_LET_FRIEND_HOST);
        add(blocked ? MPCT_L_NEARBY_STILL : MPCT_L_TRY_LOCAL);
    } else if (hostMaybe || hostCgnat) {
        v.headline = MPCT_H_JOIN_ONLY;
        add(hostCgnat ? MPCT_L_HOST_CGNAT : MPCT_L_HOST_MAYBE);
        add(MPCT_L_LET_FRIEND_HOST);
        if (blocked) add(MPCT_L_LOCAL_BLOCKED);
    } else if (blocked) {
        v.headline = MPCT_H_ONLINE_ONLY;
        if (h == MPCT_R_HOST_OPEN) add(MPCT_L_HOST_AND_JOIN);
        add(MPCT_L_LOCAL_BLOCKED);
    } else {
        v.headline = MPCT_H_ALL_GOOD;
        if (h == MPCT_R_HOST_OPEN) add(MPCT_L_HOST_AND_JOIN);
    }
    v.good = v.headline == MPCT_H_ALL_GOOD;
    return v;
}

// ---- the run ----------------------------------------------------------------------------------------------------------------------
MpConnTest::MpConnTest() {
    memset(&m_p, 0, sizeof m_p);
    m_o = MpCtOptions();
    memset(&m_view, 0, sizeof m_view);
    m_step = MPCT_CHECKS;
    m_start = m_stepStart = m_stepEnd = 0;
    m_stepBegun = false;
    m_infoRunning = m_mapAsked = m_mapHeld = m_stunAsked = false;
    m_infoMs0 = -1;
    m_samples = 0, m_sampleMin = -1;
    m_stunState = 0, m_stunIp = 0;
}

uint64_t MpConnTest::deadline(uint32_t stepMs) const {
    const uint64_t a = m_stepStart + stepMs, b = m_start + MP_CT_BUDGET_MS;
    return a < b ? a : b;
}

void MpConnTest::release() {
    if (m_infoRunning && m_p.infoCancel) m_p.infoCancel(m_p.ctx);
    m_infoRunning = false;
    if (m_stunAsked && m_p.stunEnd) m_p.stunEnd(m_p.ctx);
    m_stunAsked = false;
    if (m_mapHeld && !m_o.inRoom && m_p.mapEnd) m_p.mapEnd(m_p.ctx);   // (inside a room the test never asked: nothing of ours to let go)
    m_mapHeld = m_mapAsked = false;
}

void MpConnTest::start(const MpCtProbes& probes, const MpCtOptions& options) {
    if (m_view.running) cancel();
    m_p = probes;
    m_o = options;
    const uint32_t serial = m_view.serial + 1;
    memset(&m_view, 0, sizeof m_view);
    m_view.serial = serial;
    for (int i = 0; i < MPCT_CHECKS; i++) m_view.detail[i] = -1;
    m_view.running = true;
    m_view.inRoom = options.inRoom;
    m_start = m_p.now ? m_p.now(m_p.ctx) : 0;
    m_infoMs0 = -1;
    begin(0);
}

void MpConnTest::cancel() {
    release();
    if (m_view.running) {
        m_view.running = false;
        m_view.done = false;
        for (int i = 0; i < MPCT_CHECKS; i++)
            if (m_view.state[i] != MPCT_ST_DONE) m_view.state[i] = MPCT_ST_WAITING;
        m_view.serial++;
    }
    m_step = MPCT_CHECKS;
}

void MpConnTest::begin(int step) {
    m_step = step;
    m_stepBegun = false;
    m_stepStart = m_p.now ? m_p.now(m_p.ctx) : m_start;
    m_samples = 0, m_sampleMin = -1;
    if (step < MPCT_CHECKS) {
        m_view.state[step] = MPCT_ST_RUNNING;
        m_view.serial++;
    }
}

void MpConnTest::finishStep(uint8_t result, int detail) {
    if (m_step >= MPCT_CHECKS) return;
    release();
    m_view.state[m_step] = MPCT_ST_DONE;
    m_view.result[m_step] = result;
    m_view.detail[m_step] = static_cast<int16_t>(detail < -1 ? -1 : (detail > 32000 ? 32000 : detail));
    m_view.serial++;
    if (m_step + 1 < MPCT_CHECKS) begin(m_step + 1);
    else finishRun();
}

void MpConnTest::finishRun() {
    release();
    m_step = MPCT_CHECKS;
    m_view.running = false;
    m_view.done = true;
    m_view.verdict = mp_ct_verdict(m_view.result);
    m_view.serial++;
}

void MpConnTest::pump() {
    if (!m_view.running) return;
    const uint64_t now = m_p.now ? m_p.now(m_p.ctx) : m_start + MP_CT_BUDGET_MS;
    if (m_p.busy) m_p.busy(m_p.ctx);
    // out of time before a step starts: it and the rest are skipped (a running step ends at its own deadline, which the budget caps)
    if (!m_stepBegun && now >= m_start + MP_CT_BUDGET_MS && m_step > MPCT_WIFI) {
        finishStep(MPCT_R_SKIPPED, -1);
        return;
    }
    switch (m_step) {
    case MPCT_WIFI: stepWifi(now); break;
    case MPCT_LOCAL: stepLocal(now); break;
    case MPCT_INTERNET: stepInternet(now); break;
    case MPCT_HOSTING: stepHosting(now); break;
    case MPCT_DELAY: stepDelay(now); break;
    default: break;
    }
}

void MpConnTest::stepWifi(uint64_t) {
    MpCtWifi w;
    memset(&w, 0, sizeof w);
    w.quality = -1;
    w.connected = true;   // (no probe: the platform cannot tell, so the other checks decide)
    if (m_p.wifi && !m_p.wifi(m_p.ctx, &w)) w.connected = true, w.quality = -1;
    const uint8_t r = mp_ct_wifi_result(w);
    finishStep(r, r == MPCT_R_WIFI_GOOD || r == MPCT_R_WIFI_WEAK ? w.quality : -1);
}

void MpConnTest::stepLocal(uint64_t now) {
    if (m_view.result[MPCT_WIFI] == MPCT_R_WIFI_NONE) {
        finishStep(MPCT_R_SKIPPED, -1);
        return;
    }
    if (!m_stepBegun) {
        m_stepBegun = true;
        m_stepEnd = deadline(MP_CT_LOCAL_MS);
    }
    const int rooms = m_p.local ? m_p.local(m_p.ctx) : -1;
    if (rooms == -2) finishStep(MPCT_R_SKIPPED, -1);
    else if (rooms < 0) finishStep(MPCT_R_LOCAL_BLOCKED, -1);   // (-1, and any other word that is not a count)
    else if (rooms > 0) finishStep(MPCT_R_LOCAL_FOUND, rooms);
    else if (now >= m_stepEnd) finishStep(MPCT_R_LOCAL_OK, 0);
}

void MpConnTest::stepInternet(uint64_t now) {
    if (!m_stepBegun) {
        m_stepBegun = true;
        if (m_view.result[MPCT_WIFI] == MPCT_R_WIFI_NONE) {
            finishStep(MPCT_R_SKIPPED, -1);
            return;
        }
        if (!m_p.serviceConfigured || !m_p.serviceConfigured(m_p.ctx)) {
            finishStep(MPCT_R_NET_NOT_SET_UP, -1);
            return;
        }
        m_stepEnd = deadline(MP_CT_INFO_MS);
        const uint64_t left = m_stepEnd > now ? m_stepEnd - now : 0;
        if (!m_p.infoBegin || left == 0 || !m_p.infoBegin(m_p.ctx, static_cast<uint32_t>(left))) {
            finishStep(MPCT_R_NET_DOWN, -1);
            return;
        }
        m_infoRunning = true;
        return;
    }
    MpCtInfo info;
    memset(&info, 0, sizeof info);
    info.outcome = MPCT_INFO_DOWN;   // (what a probe that says nothing means)
    const int got = m_p.infoPoll ? m_p.infoPoll(m_p.ctx, &info) : 1;
    if (got == 0) {
        if (now >= m_stepEnd) finishStep(MPCT_R_NET_DOWN, -1);   // (release() cancels the call)
        return;
    }
    m_infoRunning = false;
    switch (info.outcome) {
    case MPCT_INFO_OK:
        m_infoMs0 = info.ms >= 0 && info.ms <= 600000 ? info.ms : -1;
        finishStep(MPCT_R_NET_OK, -1);
        break;
    case MPCT_INFO_KILLED: finishStep(MPCT_R_NET_OFF, -1); break;
    case MPCT_INFO_UPDATE: finishStep(MPCT_R_NET_UPDATE, -1); break;
    default: finishStep(MPCT_R_NET_DOWN, -1); break;   // (down, offline, and any code nobody knows)
    }
}

void MpConnTest::stepHosting(uint64_t now) {
    const bool netOk = m_view.result[MPCT_INTERNET] == MPCT_R_NET_OK;
    if (!m_stepBegun) {
        m_stepBegun = true;
        m_stepEnd = deadline(MP_CT_HOST_MS);
        m_stunState = 0, m_stunIp = 0;
        if (!netOk) {
            finishStep(MPCT_R_SKIPPED, -1);
            return;
        }
        if (m_o.inRoom) {
            // a room is open: only read what a hosting room already holds (the router mapping is the room's, never ours to ask or release)
            if (!m_o.isHost || !m_p.mapPoll) {
                finishStep(MPCT_R_SKIPPED, -1);
                return;
            }
            MpCtMap map;
            memset(&map, 0, sizeof map);
            m_p.mapPoll(m_p.ctx, &map);
            finishStep(mp_ct_host_result(true, map, false, 0, 0), -1);
            return;
        }
        if (m_o.relayOnly) {
            // relay only: no address of the player's leaves the console, so neither the router (UPnP / NAT-PMP) nor a STUN server is asked.
            // Hosting is not tested (an online room then runs over the relay; the Details window says "Relay only is on").
            finishStep(MPCT_R_SKIPPED, -1);
            return;
        }
        m_mapAsked = m_p.mapBegin && m_p.mapBegin(m_p.ctx) == 1;
        m_mapHeld = m_mapAsked;
        m_stunAsked = m_p.stunBegin && m_p.stunBegin(m_p.ctx) == 1;
        if (!m_mapAsked && !m_stunAsked) {   // both off: nothing to learn
            finishStep(MPCT_R_HOST_UNKNOWN, -1);
            return;
        }
        return;
    }
    MpCtMap map;
    memset(&map, 0, sizeof map);
    if (m_mapAsked && m_p.mapPoll) m_p.mapPoll(m_p.ctx, &map);
    if (m_stunAsked && m_stunState == 0 && m_p.stunPoll) m_stunState = m_p.stunPoll(m_p.ctx, &m_stunIp);
    const bool mapSettled = !m_mapAsked || map.state != MPCT_MAP_WORKING;
    const bool stunSettled = !m_stunAsked || m_stunState != 0;
    if ((mapSettled && stunSettled) || now >= m_stepEnd) {
        const uint8_t result = mp_ct_host_result(m_mapAsked, map, m_stunAsked, m_stunState, m_stunIp);
        finishStep(result, -1);   // (release() lets the mapping go)
    }
}

void MpConnTest::stepDelay(uint64_t now) {
    if (!m_stepBegun) {
        m_stepBegun = true;
        m_stepEnd = deadline(MP_CT_RTT_SAMPLES * MP_CT_RTT_MS + 200);
        // inside a room the delay that matters is the one to the room's other end
        if (m_o.inRoom && m_p.hostRtt) {
            const int ms = m_p.hostRtt(m_p.ctx);
            const uint8_t r = mp_ct_delay_result(ms);
            if (r != MPCT_R_NONE) {
                finishStep(r, ms);
                return;
            }
        }
        if (m_view.result[MPCT_INTERNET] != MPCT_R_NET_OK || !m_p.infoBegin) {
            finishStep(MPCT_R_SKIPPED, -1);
            return;
        }
        const uint64_t left = m_stepEnd > now ? m_stepEnd - now : 0;
        const uint32_t t = left < MP_CT_RTT_MS ? static_cast<uint32_t>(left) : MP_CT_RTT_MS;
        if (t == 0 || !m_p.infoBegin(m_p.ctx, t)) {
            const uint8_t r = mp_ct_delay_result(m_infoMs0);   // (the first call's time: it includes the handshake, so it is the pessimistic one)
            finishStep(r == MPCT_R_NONE ? uint8_t(MPCT_R_SKIPPED) : r, m_infoMs0);
            return;
        }
        m_infoRunning = true;
        return;
    }
    MpCtInfo info;
    memset(&info, 0, sizeof info);
    info.outcome = MPCT_INFO_DOWN;
    const int got = m_p.infoPoll ? m_p.infoPoll(m_p.ctx, &info) : 1;
    if (got == 0 && now < m_stepEnd) return;
    if (got == 0 && m_p.infoCancel) m_p.infoCancel(m_p.ctx);
    m_infoRunning = false;
    if (got == 1 && info.outcome == MPCT_INFO_OK && info.ms >= 0 && info.ms <= 600000) {
        ++m_samples;
        if (m_sampleMin < 0 || info.ms < m_sampleMin) m_sampleMin = info.ms;
    }
    const bool more = got == 1 && m_samples < MP_CT_RTT_SAMPLES && now < m_stepEnd;
    if (more) {
        const uint64_t left = m_stepEnd - now;
        const uint32_t t = left < MP_CT_RTT_MS ? static_cast<uint32_t>(left) : MP_CT_RTT_MS;
        if (t > 0 && m_p.infoBegin(m_p.ctx, t)) {
            m_infoRunning = true;
            return;
        }
    }
    const int ms = m_sampleMin >= 0 ? m_sampleMin : m_infoMs0;
    const uint8_t r = mp_ct_delay_result(ms);
    finishStep(r == MPCT_R_NONE ? uint8_t(MPCT_R_SKIPPED) : r, r == MPCT_R_NONE ? -1 : ms);
}
