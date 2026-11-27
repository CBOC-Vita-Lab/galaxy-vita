// Galaxy 3.0 multiplayer (port side, owner: core): one reliable, ordered message channel between two consoles (one hop).
//
// Sender: every message gets the next 16-bit sequence number; at most kWindow messages are in flight (sent, not acknowledged);
// more wait in the backlog (kOutEntries messages / kOutBytes bytes in all). An unacknowledged message is sent again after the
// retransmission timeout (max(2*RTT, 60 ms), doubling with every further try up to 4x, never above 2 s; resends are also capped per peer in bytes/s); a message the receiver
// skipped (it acknowledged later ones selectively) is sent again at once (fast retransmit, at most every half timeout).
// Receiver: duplicate suppression and in-order delivery; up to kWindow-1 messages ahead of the next expected one are kept (a
// message that does not fit is not acknowledged, so it comes again).
// Acknowledgement (in every packet header): ack = the last sequence delivered in order (cumulative); bit i of ackbits = sequence
// ack+2+i is held out of order (ack+1 is by definition missing).
#ifndef PORT_MP_RELIABLE_H
#define PORT_MP_RELIABLE_H

#include <stddef.h>
#include <stdint.h>

#include "port/mp_proto.h"

struct MpRelMsg {
    uint16_t seq;
    uint8_t type;
    uint8_t from;
    uint16_t len;
    const uint8_t* data;
};

class MpReliable {
public:
    enum { kWindow = 64, kOutEntries = 1024,   // core2: 1024 (was 512): a 15 s freeze at a star-bit storm's ~40 events/s (DESIGN 3)
            kOutBytes = 32 * 1024, kSmall = 128, kBig = 16, kMinRtoMs = 60, kMaxRtoMs = 2000,
           kResendBytesPerSec = 24 * 1024, kResendBurst = 8 * 1024 };   // netpack: resend byte budget per peer

    static void setMaxBackoffShift(int shift);   // tests: the backoff ceiling as a shift of the rto (default 2 = 4x)
    void reset(uint16_t firstSendSeq);   // also forgets the receive side
    // ---- send
    bool push(uint8_t type, uint8_t from, const void* data, uint16_t len);   // false: backlog full (nothing queued)
    // Calls emit(ctx, msg) for each message due for (first or repeated) sending, oldest first, at most `budget`; emit returns
    // false when it could not send (stop). Returns how many were emitted.
    int emitDue(uint64_t nowMs, uint32_t rtoMs, int budget, bool (*emit)(void* ctx, const MpRelMsg& m), void* ctx);
    // Header ack fields from the other side. Returns an RTT sample in ms (first transmissions only), or -1.
    int onAck(uint16_t ack, uint16_t bits, uint64_t nowMs);
    int unacked() const { return m_count; }   // messages queued and not yet acknowledged
    bool everythingAcked() const { return m_count == 0; }
    uint32_t resends() const { return m_resends; }

    // ---- receive
    bool recvStarted() const { return m_recvStarted; }
    void startRecv(uint16_t firstSeq);
    // A reliable message arrived. Delivers it and every held message that now follows in order through deliver(ctx, msg).
    // Returns 1 accepted (delivered or held), 0 duplicate (acknowledge again), -1 not accepted (outside the window / no room;
    // do not acknowledge).
    int receive(const MpRelMsg& m, void (*deliver)(void* ctx, const MpRelMsg& m), void* ctx);
    // core2: back-pressure. With a gate set, a message the gate refuses is not delivered: an in-order one is not acknowledged (it comes
    // again), a held one stays held; drainHeld delivers what the gate now lets through (call it every pump).
    void setGate(bool (*accept)(void* ctx, const MpRelMsg& m)) { m_gate = accept; }
    void drainHeld(void (*deliver)(void* ctx, const MpRelMsg& m), void* ctx);
    void ackFields(uint16_t* ack, uint16_t* bits) const;
    uint16_t expected() const { return m_expect; }
    bool ackDirty;   // something arrived that the other side should hear about soon

private:
    struct Out {
        uint16_t seq, len;
        uint8_t type, from, acked, sends, fast;
        uint32_t off, span;
        uint64_t firstSend, lastSend;
    };
    struct Slot {
        bool used;
        uint16_t seq, len;
        uint8_t type, from;
        int8_t big;   // index into m_big, or -1 (inline)
        uint8_t small[kSmall];
    };

    // send side
    Out m_out[kOutEntries];
    int m_outHead, m_count;          // ring of queued messages (oldest = head)
    uint16_t m_nextSeq;
    uint8_t m_bytes[kOutBytes];
    uint32_t m_byteHead, m_byteTail, m_byteUsed;
    uint32_t m_resends;
    uint32_t m_lastRto;
    int32_t m_tokens;      // netpack: resend byte budget (token bucket)
    uint64_t m_tokAt;
    // receive side
    bool m_recvStarted;
    uint16_t m_expect;
    bool (*m_gate)(void* ctx, const MpRelMsg& m) = 0;
    Slot m_slots[kWindow];
    uint8_t m_big[kBig][MP_MAX_PAYLOAD];
    bool m_bigUsed[kBig];

    bool alloc(uint32_t len, uint32_t* off, uint32_t* span);
    void popAcked();
};

#endif  // PORT_MP_RELIABLE_H
