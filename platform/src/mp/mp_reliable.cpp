// Galaxy 3.0 multiplayer (port side, owner: core): reliable ordered channel. See mp_reliable.h.
#include "mp_reliable.h"

#include <string.h>

static inline int16_t SeqDiff(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b); }

static int s_maxShift = 2;
void MpReliable::setMaxBackoffShift(int shift) { s_maxShift = shift < 0 ? 0 : shift > 6 ? 6 : shift; }

void MpReliable::reset(uint16_t firstSendSeq) {
    m_outHead = 0;
    m_count = 0;
    m_nextSeq = firstSendSeq;
    m_byteHead = m_byteTail = m_byteUsed = 0;
    m_resends = 0;
    m_lastRto = 200;
    m_tokens = kResendBurst;
    m_tokAt = 0;
    m_recvStarted = false;
    m_expect = 0;
    ackDirty = false;
    for (int i = 0; i < kWindow; i++) m_slots[i].used = false;
    for (int i = 0; i < kBig; i++) m_bigUsed[i] = false;
}

bool MpReliable::alloc(uint32_t len, uint32_t* off, uint32_t* span) {
    if (len == 0) {
        *off = m_byteTail;
        *span = 0;
        return true;
    }
    if (m_byteUsed == 0) m_byteHead = m_byteTail = 0;
    if (m_byteUsed >= kOutBytes) return false;
    if (m_byteTail >= m_byteHead && !(m_byteUsed > 0 && m_byteTail == m_byteHead)) {
        // free: [tail, end) and [0, head)
        if (len <= kOutBytes - m_byteTail) {
            *off = m_byteTail;
            *span = len;
        } else if (len <= m_byteHead) {
            *off = 0;
            *span = (kOutBytes - m_byteTail) + len;
        } else {
            return false;
        }
    } else {
        // free: [tail, head)
        if (len > m_byteHead - m_byteTail) return false;
        *off = m_byteTail;
        *span = len;
    }
    m_byteTail = *off + len;
    if (m_byteTail == kOutBytes) m_byteTail = 0;
    m_byteUsed += *span;
    return true;
}

bool MpReliable::push(uint8_t type, uint8_t from, const void* data, uint16_t len) {
    if (m_count >= kOutEntries || len > MP_MAX_PAYLOAD) return false;
    uint32_t off = 0, span = 0;
    if (!alloc(len, &off, &span)) return false;
    Out& o = m_out[(m_outHead + m_count) % kOutEntries];
    o.seq = m_nextSeq++;
    o.len = len;
    o.type = type;
    o.from = from;
    o.acked = 0;
    o.sends = 0;
    o.fast = 0;
    o.off = off;
    o.span = span;
    o.firstSend = o.lastSend = 0;
    if (len) memcpy(m_bytes + off, data, len);
    m_count++;
    return true;
}

int MpReliable::emitDue(uint64_t nowMs, uint32_t rtoMs, int budget, bool (*emit)(void*, const MpRelMsg&), void* ctx) {
    if (rtoMs < kMinRtoMs) rtoMs = kMinRtoMs;
    if (rtoMs > kMaxRtoMs) rtoMs = kMaxRtoMs;
    m_lastRto = rtoMs;
    int n = 0;
    bool starved = false;
    const int window = m_count < kWindow ? m_count : kWindow;
    for (int i = 0; i < window && n < budget; i++) {
        Out& o = m_out[(m_outHead + i) % kOutEntries];
        if (o.acked) continue;
        if (o.sends) {
            // MP3 netpack: exponential backoff per message (rto, 2x, 4x; never above kMaxRtoMs). Not 8x / 16x: a partition that heals must be noticed
            // inside the 4 s a claiming host waits (MP_CLAIM_MS; its peers start at rto 200 ms); the byte budget below does the rest
            const int shift = o.sends - 1 < s_maxShift ? o.sends - 1 : s_maxShift;
            uint32_t rto = rtoMs << shift;
            if (rto > kMaxRtoMs) rto = kMaxRtoMs;
            if (!o.fast && (nowMs > o.lastSend ? nowMs - o.lastSend : 0) < rto) continue;
        }
        if (o.sends) {
            // MP3 netpack: resend byte budget per peer (a dead or cut-off console must not eat the Wi-Fi of the others)
            if (nowMs > m_tokAt) {
                const uint64_t add = (nowMs - m_tokAt) * kResendBytesPerSec / 1000;
                m_tokens = (int32_t)(m_tokens + add > (uint64_t)kResendBurst ? (uint64_t)kResendBurst : m_tokens + add);
                m_tokAt = nowMs;
            }
            const int32_t cost = (int32_t)o.len + 24;
            if (starved || m_tokens < cost) { starved = true; continue; }   // (oldest first: a later, smaller message must not take the tokens an older one waits for)
            m_tokens -= cost;
        }
        MpRelMsg m;
        m.seq = o.seq;
        m.type = o.type;
        m.from = o.from;
        m.len = o.len;
        m.data = m_bytes + o.off;
        if (!emit(ctx, m)) break;
        o.fast = 0;
        if (o.sends == 0) o.firstSend = nowMs;
        else m_resends++;
        if (o.sends < 255) o.sends++;
        o.lastSend = nowMs;
        n++;
    }
    return n;
}

void MpReliable::popAcked() {
    while (m_count > 0 && m_out[m_outHead].acked) {
        Out& o = m_out[m_outHead];
        m_byteUsed -= o.span;
        if (o.len) {
            m_byteHead = o.off + o.len;
            if (m_byteHead == kOutBytes) m_byteHead = 0;
        }
        m_outHead = (m_outHead + 1) % kOutEntries;
        m_count--;
    }
    if (m_count == 0) m_byteHead = m_byteTail = m_byteUsed = 0;
}

int MpReliable::onAck(uint16_t ack, uint16_t bits, uint64_t nowMs) {
    if (m_count == 0) return -1;
    const uint16_t headSeq = m_out[m_outHead].seq;
    int sample = -1;
    // cumulative part: every queued message with seq <= ack (only when ack is inside [head-1, head+count-1])
    const int16_t upto = SeqDiff(ack, headSeq);   // index of the acked message relative to head
    if (upto >= -1 && upto < m_count) {
        for (int i = 0; i <= upto; i++) {
            Out& o = m_out[(m_outHead + i) % kOutEntries];
            if (!o.acked && o.sends) {
                o.acked = 1;
                if (o.sends == 1) {
                    const int s = nowMs > o.firstSend ? (int)(nowMs - o.firstSend) : 0;
                    if (sample < 0 || s < sample) sample = s;
                }
            }
        }
        int highest = -1;
        for (int b = 0; b < 16; b++) {
            if (!(bits & (1u << b))) continue;
            highest = upto + 2 + b;
            const int idx = upto + 2 + b;
            if (idx < 0 || idx >= m_count) continue;
            Out& o = m_out[(m_outHead + idx) % kOutEntries];
            if (!o.acked && o.sends) {
                o.acked = 1;
                if (o.sends == 1) {
                    const int s = nowMs > o.firstSend ? (int)(nowMs - o.firstSend) : 0;
                    if (sample < 0 || s < sample) sample = s;
                }
            }
        }
        // fast retransmit: the receiver holds later messages, so the ones it skipped were lost
        for (int idx = upto + 1; idx < highest && idx < m_count; idx++) {
            if (idx < 0) continue;
            Out& o = m_out[(m_outHead + idx) % kOutEntries];
            if (!o.acked && o.sends && (nowMs > o.lastSend ? nowMs - o.lastSend : 0) >= m_lastRto / 2) o.fast = 1;
        }
    }
    popAcked();
    return sample;
}

void MpReliable::startRecv(uint16_t firstSeq) {
    m_recvStarted = true;
    m_expect = firstSeq;
    for (int i = 0; i < kWindow; i++) m_slots[i].used = false;
    for (int i = 0; i < kBig; i++) m_bigUsed[i] = false;
}

int MpReliable::receive(const MpRelMsg& m, void (*deliver)(void*, const MpRelMsg&), void* ctx) {
    if (!m_recvStarted || m.len > MP_MAX_PAYLOAD) return -1;
    const int16_t d = SeqDiff(m.seq, m_expect);
    if (d < 0) {
        ackDirty = true;
        return 0;
    }
    if (d >= kWindow) return -1;
    if (d > 0) {
        Slot& s = m_slots[m.seq % kWindow];
        if (s.used) {
            ackDirty = true;
            return s.seq == m.seq ? 0 : -1;
        }
        int8_t big = -1;
        if (m.len > kSmall) {
            for (int i = 0; i < kBig; i++)
                if (!m_bigUsed[i]) { big = (int8_t)i; break; }
            if (big < 0) return -1;   // no room: not acknowledged, it comes again
            m_bigUsed[big] = true;
            memcpy(m_big[big], m.data, m.len);
        } else if (m.len) {
            memcpy(s.small, m.data, m.len);
        }
        s.used = true;
        s.seq = m.seq;
        s.len = m.len;
        s.type = m.type;
        s.from = m.from;
        s.big = big;
        ackDirty = true;
        return 1;
    }
    // in order
    if (m_gate && !m_gate(ctx, m)) return -1;   // core2: no room on the receiving side: not acknowledged, it comes again
    // netfix B4: a held copy of this very message (it was acknowledged, then refused by the gate and stayed held) is no longer wanted:
    // free its slot and big block now, or the slot stays "used" with this seq for ever
    {
        Slot& hs = m_slots[m.seq % kWindow];
        if (hs.used && hs.seq == m.seq) {
            if (hs.big >= 0) m_bigUsed[hs.big] = false;
            hs.used = false;
        }
    }
    m_expect++;
    ackDirty = true;
    deliver(ctx, m);
    drainHeld(deliver, ctx);
    return 1;
}

void MpReliable::drainHeld(void (*deliver)(void*, const MpRelMsg&), void* ctx) {
    for (;;) {
        if (!m_recvStarted) break;
        Slot& s = m_slots[m_expect % kWindow];
        if (!s.used || s.seq != m_expect) break;
        MpRelMsg h;
        h.seq = s.seq;
        h.type = s.type;
        h.from = s.from;
        h.len = s.len;
        h.data = s.big >= 0 ? m_big[s.big] : s.small;
        if (m_gate && !m_gate(ctx, h)) break;   // core2 (held: already acknowledged; delivered on a later drainHeld)
        m_expect++;
        ackDirty = true;
        deliver(ctx, h);
        // (deliver copies the message out; the slot is free after it returns)
        if (s.big >= 0) m_bigUsed[s.big] = false;
        s.used = false;
        if (!m_recvStarted) break;   // the delivery ended the channel (reset from inside deliver)
    }
}

void MpReliable::ackFields(uint16_t* ack, uint16_t* bits) const {
    *ack = (uint16_t)(m_expect - 1);
    uint16_t b = 0;
    if (m_recvStarted) {
        for (int i = 0; i < 16; i++) {
            const uint16_t seq = (uint16_t)(m_expect + 1 + i);
            const Slot& s = m_slots[seq % kWindow];
            if (s.used && s.seq == seq) b |= (uint16_t)(1u << i);
        }
    }
    *bits = b;
}
