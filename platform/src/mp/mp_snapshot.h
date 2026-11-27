// Galaxy 3.0 multiplayer (port side, owner: core): transfer of the host's progress snapshot (the user-file blocks; the save
// module makes and applies the bytes). Host -> guest over the reliable channel:
//   SNAPSHOT_BEGIN {total u32, crc32 u32}   total 1..MP_SNAPSHOT_MAX
//   SNAPSHOT_CHUNK {offset u16, len u16, bytes[len]}   len 1..MP_SNAPSHOT_CHUNK, offsets contiguous from 0 (the channel is in order)
//   guest -> host: SNAPSHOT_DONE {crc32 u32, ok u8}
// Every console keeps the latest complete image (host: offered by the game; guest: received) so that any console can serve it
// after a host migration (DESIGN.md section 8).
#ifndef PORT_MP_SNAPSHOT_H
#define PORT_MP_SNAPSHOT_H

#include <stdint.h>

#include "port/mp_proto.h"

struct MpSnapshotImage {
    uint8_t bytes[MP_SNAPSHOT_MAX];
    uint32_t len;    // 0 = none
    uint32_t crc;
    uint32_t version;   // local counter: bumps on every new image
};

class MpSnapshotRx {
public:
    void reset() { m_total = m_got = m_crc = 0; m_active = false; }
    bool active() const { return m_active; }
    // Each returns false on a malformed or out-of-sequence message (the transfer is dropped).
    bool begin(const uint8_t* payload, uint16_t len);
    // true + *complete when the last chunk arrived; then crcOk() says whether the image is good.
    bool chunk(const uint8_t* payload, uint16_t len, bool* complete);
    bool crcOk() const;
    uint32_t total() const { return m_total; }
    uint32_t got() const { return m_got; }
    uint32_t expectedCrc() const { return m_crc; }
    const uint8_t* bytes() const { return m_buf; }
    int progress() const { return m_total ? (int)(m_got * 100u / m_total) : 0; }
private:
    uint8_t m_buf[MP_SNAPSHOT_MAX];
    uint32_t m_total, m_got, m_crc;
    bool m_active;
};

// Payload builders. Return the payload length (0 on bad arguments).
uint16_t mp_snapshot_begin_payload(uint8_t* out, uint16_t cap, uint32_t total, uint32_t crc);
uint16_t mp_snapshot_chunk_payload(uint8_t* out, uint16_t cap, const uint8_t* image, uint32_t total, uint32_t offset);
uint16_t mp_snapshot_done_payload(uint8_t* out, uint16_t cap, uint32_t crc, bool ok);
bool mp_snapshot_done_read(const uint8_t* payload, uint16_t len, uint32_t* crc, bool* ok);

#endif  // PORT_MP_SNAPSHOT_H
