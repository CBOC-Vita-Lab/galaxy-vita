// optfix item 15: the held-message pool (decomp/include/Game/MP/MpHeldPool.hpp).   bash tests/mp/held_pool_build.sh
#include <stdio.h>
#include <string.h>

#include "Game/MP/MpHeldPool.hpp"

static int gChecks, gFails;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static MpMsg make(uint8_t type, uint8_t from, uint16_t len, uint8_t fill) {
    MpMsg m;
    memset(&m, 0, sizeof m);
    m.type = type; m.fromSlot = from; m.len = len;
    memset(m.data, fill, len);
    return m;
}

int main() {
    static MpHeld::Pool p;
    CHECK(sizeof(p) < 8300);   // (was 24 x 1028 = 24672 bytes)
    MpMsg out;
    p.rewind();
    CHECK(!p.next(&out) && p.count() == 0);
    // order and content kept, real lengths
    CHECK(p.push(make(0x40, 1, 4, 0xA1)));
    CHECK(p.push(make(0x41, 2, 0, 0)));
    CHECK(p.push(make(0x42, 3, 600, 0xB2)));
    CHECK(p.push(make(0x21, 0, MP_MAX_PAYLOAD, 0xC3)));
    CHECK(p.count() == 4);
    p.rewind();
    CHECK(p.next(&out) && out.type == 0x40 && out.fromSlot == 1 && out.len == 4 && out.data[0] == 0xA1 && out.data[3] == 0xA1);
    CHECK(p.next(&out) && out.type == 0x41 && out.len == 0);
    CHECK(p.next(&out) && out.type == 0x42 && out.len == 600 && out.data[599] == 0xB2);
    CHECK(p.next(&out) && out.type == 0x21 && out.len == MP_MAX_PAYLOAD && out.data[MP_MAX_PAYLOAD - 1] == 0xC3);
    CHECK(!p.next(&out));
    p.clear();
    CHECK(p.count() == 0 && p.usedBytes() == 0);
    // 24 small messages all fit (what the old queue held)
    for (int i = 0; i < MpHeld::MAX_ENTRIES; i++) CHECK(p.push(make(0x40, i & 3, 8, (uint8_t)i)));
    CHECK(!p.push(make(0x40, 0, 4, 0)));   // the 25th is refused, as a full queue refused it
    p.rewind();
    for (int i = 0; i < MpHeld::MAX_ENTRIES; i++) CHECK(p.next(&out) && out.data[0] == (uint8_t)i && out.fromSlot == (i & 3));
    p.clear();
    // big messages fill the pool before 24 entries: refused cleanly, nothing half-stored, the earlier ones intact
    int stored = 0;
    while (p.push(make(0x42, 1, MP_MAX_PAYLOAD, 0x5A))) stored++;
    CHECK(stored >= 7 && stored < MpHeld::MAX_ENTRIES);
    CHECK(p.usedBytes() <= MpHeld::POOL_BYTES);
    p.rewind();
    int read = 0;
    while (p.next(&out)) { CHECK(out.len == MP_MAX_PAYLOAD && out.data[0] == 0x5A); read++; }
    CHECK(read == stored);
    // an over-long len field is clamped, never read past the payload
    p.clear();
    MpMsg bad = make(0x40, 0, 8, 1);
    bad.len = 65000;
    CHECK(p.push(bad));
    p.rewind();
    CHECK(p.next(&out) && out.len == MP_MAX_PAYLOAD);
    // a small one still fits after a refused big one
    p.clear();
    while (p.push(make(0x42, 1, MP_MAX_PAYLOAD, 0))) {}
    CHECK(p.push(make(0x40, 0, 4, 7)) || p.usedBytes() + MpHeld::HEADER + 4 > MpHeld::POOL_BYTES);
    // dropRange compacts: order of the rest kept, the room is free at once, a full pool takes a new message afterwards
    p.clear();
    CHECK(p.push(make(0x13, 1, 5, 0x11)) && p.push(make(0x46, 2, 100, 0x22)) && p.push(make(0x21, 3, 7, 0x33)) && p.push(make(0x40, 0, 4, 0x44)) && p.push(make(0x22, 1, 0, 0)));
    CHECK(p.dropRange(0x40, 0x4F) == 2 && p.count() == 3 && p.usedBytes() == (uint32_t)(3 * MpHeld::HEADER + 5 + 7));
    p.rewind();
    CHECK(p.next(&out) && out.type == 0x13 && out.len == 5 && out.data[0] == 0x11);
    CHECK(p.next(&out) && out.type == 0x21 && out.len == 7 && out.data[6] == 0x33);
    CHECK(p.next(&out) && out.type == 0x22 && out.len == 0 && !p.next(&out));
    p.clear();
    while (p.push(make(0x46, 1, 300, 5))) {}
    CHECK(!p.push(make(0x46, 1, 300, 5)) && p.dropRange(0x40, 0x4F) > 0 && p.count() == 0 && p.usedBytes() == 0 && p.push(make(0x46, 1, 300, 5)));
    printf("held_pool_test: %d checks, %d failed\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
