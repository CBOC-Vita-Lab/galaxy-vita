// Galaxy 3.0 multiplayer (port side, owner: core): snapshot transfer codec and receiver. See mp_snapshot.h.
#include "mp_snapshot.h"

#include "mp_core_sys.h"

#include <string.h>

bool MpSnapshotRx::begin(const uint8_t* payload, uint16_t len) {
    MpReader r(payload, len);
    const uint32_t total = r.u32();
    const uint32_t crc = r.u32();
    m_active = false;
    if (!r.done() || total == 0 || total > MP_SNAPSHOT_MAX) return false;
    m_total = total;
    m_crc = crc;
    m_got = 0;
    m_active = true;
    return true;
}

bool MpSnapshotRx::chunk(const uint8_t* payload, uint16_t len, bool* complete) {
    *complete = false;
    if (!m_active) return false;
    MpReader r(payload, len);
    const uint16_t off = r.u16();
    const uint16_t n = r.u16();
    if (!r.ok() || n == 0 || n > MP_SNAPSHOT_CHUNK || off != m_got || (uint32_t)off + n > m_total || len != 4u + n) {
        m_active = false;
        return false;
    }
    memcpy(m_buf + off, payload + 4, n);
    m_got += n;
    if (m_got == m_total) {
        *complete = true;
        m_active = false;
    }
    return true;
}

bool MpSnapshotRx::crcOk() const { return m_total && m_got == m_total && mp_crc32(0, m_buf, m_total) == m_crc; }

uint16_t mp_snapshot_begin_payload(uint8_t* out, uint16_t cap, uint32_t total, uint32_t crc) {
    MpWriter w(out, cap);
    w.u32(total);
    w.u32(crc);
    return w.ok() ? (uint16_t)w.n : 0;
}

uint16_t mp_snapshot_chunk_payload(uint8_t* out, uint16_t cap, const uint8_t* image, uint32_t total, uint32_t offset) {
    if (offset >= total || total > MP_SNAPSHOT_MAX) return 0;
    uint32_t n = total - offset;
    if (n > MP_SNAPSHOT_CHUNK) n = MP_SNAPSHOT_CHUNK;
    MpWriter w(out, cap);
    w.u16((uint16_t)offset);
    w.u16((uint16_t)n);
    w.bytes(image + offset, n);
    return w.ok() ? (uint16_t)w.n : 0;
}

uint16_t mp_snapshot_done_payload(uint8_t* out, uint16_t cap, uint32_t crc, bool ok) {
    MpWriter w(out, cap);
    w.u32(crc);
    w.u8(ok ? 1 : 0);
    return w.ok() ? (uint16_t)w.n : 0;
}

bool mp_snapshot_done_read(const uint8_t* payload, uint16_t len, uint32_t* crc, bool* ok) {
    MpReader r(payload, len);
    *crc = r.u32();
    const uint8_t v = r.u8();
    *ok = v == 1;
    return r.done() && v <= 1;
}
