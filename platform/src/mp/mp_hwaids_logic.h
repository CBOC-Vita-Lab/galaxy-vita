// Galaxy 3.0 multiplayer, first-console aids (hwaids): the pure logic of the UDP log mirror, with no system calls so a host test
// can run it: the "a.b.c.d:port" target parser and a fixed-size ring of whole log lines (oldest lines are dropped when it is full).
#ifndef PORT_MP_HWAIDS_LOGIC_H
#define PORT_MP_HWAIDS_LOGIC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mpaids {

// "192.168.1.20:9999" (spaces around are fine). Strict: four numbers 0..255 without leading junk, a port 1..65535, nothing after.
inline bool ParseTarget(const char* s, uint8_t ip[4], uint16_t* port) {
    if (!s) return false;
    while (*s == ' ' || *s == '\t') s++;
    unsigned v[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < 5; i++) {
        if (*s < '0' || *s > '9') return false;
        unsigned digits = 0;
        unsigned n = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (unsigned)(*s - '0');
            if (++digits > 5 || n > 65535) return false;
            s++;
        }
        v[i] = n;
        if (i < 3) {
            if (*s != '.') return false;
            s++;
        } else if (i == 3) {
            if (*s != ':') return false;
            s++;
        }
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s != 0) return false;
    for (int i = 0; i < 4; i++) {
        if (v[i] > 255) return false;
        ip[i] = (uint8_t)v[i];
    }
    if (v[4] == 0) return false;
    *port = (uint16_t)v[4];
    return true;
}

// Only private addresses are accepted as the mirror's target (nothing may leave the owner's own network): 10/8, 172.16/12, 192.168/16,
// 127/8. A directed broadcast (last byte 255) is refused as well.
inline bool IsPrivateTarget(const uint8_t ip[4]) {
    if (ip[3] == 255) return false;
    if (ip[0] == 10 || ip[0] == 127) return true;
    if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) return true;
    return ip[0] == 192 && ip[1] == 168;
}

// A ring of whole lines: each is stored as a 2-byte length and its bytes (no newline). Full: the oldest lines go (counted).
// Not thread-safe: the caller holds a lock.
template <size_t N>
struct LineRing {
    static_assert(N >= 2048 && N <= 65536, "ring size");
    static constexpr size_t kMaxLine = 1000;   // a longer line is cut
    uint8_t buf[N];
    size_t head = 0;      // where the oldest line starts
    size_t used = 0;      // bytes in use
    unsigned dropped = 0; // lines lost to a full ring since the last TakeDropped

    void Put(size_t at, uint8_t b) { buf[at % N] = b; }
    uint8_t Get(size_t at) const { return buf[at % N]; }
    size_t LenAt(size_t at) const { return (size_t)Get(at) | ((size_t)Get(at + 1) << 8); }

    void Push(const char* line, size_t len) {
        if (len > kMaxLine) len = kMaxLine;
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
        if (len == 0) return;
        const size_t need = len + 2;
        while (used + need > N && used > 0) {
            const size_t old = LenAt(head) + 2;
            head = (head + old) % N;
            used -= old;
            dropped++;
        }
        const size_t tail = (head + used) % N;
        Put(tail, (uint8_t)(len & 0xFF));
        Put(tail + 1, (uint8_t)(len >> 8));
        for (size_t i = 0; i < len; i++) Put(tail + 2 + i, (uint8_t)line[i]);
        used += need;
    }

    bool Empty() const { return used == 0; }

    // Moves whole lines (each followed by '\n') into out while they fit in cap bytes; returns the byte count. A line that alone is
    // bigger than cap cannot happen (kMaxLine + 1 <= cap is required of the caller).
    size_t PopBatch(char* out, size_t cap) {
        size_t n = 0;
        while (used > 0) {
            const size_t len = LenAt(head);
            if (n + len + 1 > cap) break;
            for (size_t i = 0; i < len; i++) out[n + i] = (char)Get(head + 2 + i);
            out[n + len] = '\n';
            n += len + 1;
            head = (head + len + 2) % N;
            used -= len + 2;
        }
        return n;
    }

    unsigned TakeDropped() {
        const unsigned d = dropped;
        dropped = 0;
        return d;
    }
};

}  // namespace mpaids

#endif
