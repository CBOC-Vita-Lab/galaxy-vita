// core2: a hand-made joiner for the core tests (ghost joiners, odd names, hostile HELLOs): the stage 3 join done by hand over the loop
// hub, HELLO1 -> COOKIE -> HELLO2 (sealed traffic after that). The HELLO2 fields can be bent per test; send() seals any packet with the
// pair key, as a member would.
#pragma once
#include "../../platform/src/mp/mp_seal.h"
#include <vector>

struct RawJoiner {
    MpAddr me{0, 0}, host{0, 0};
    uint8_t sk[32], pk[32], nonce[8], hostPk[32], cookie[16], key[32], token[16];
    uint8_t psk[32] = {0};
    uint32_t sid = 0;
    uint8_t hostSlot = 0;
    bool haveCookie = false;
    uint64_t ctr = 1;
    std::vector<std::vector<uint8_t>> got;   // what the host sent to this endpoint (sealed ones opened), in order
    static uint32_t Rnd() { static uint32_t x = 0x9E3779B9u; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }

    RawJoiner(uint32_t endpoint, const MpAddr& hostAddr) {
        me = MpAddr{endpoint, MP_SESSION_PORT};
        host = hostAddr;
        mpseal::KeyPair kp;
        mpseal::MakeKeyPair(&kp, Rnd);
        memcpy(sk, kp.sk, 32);
        memcpy(pk, kp.pk, 32);
        for (int i = 0; i < 8; i++) nonce[i] = (uint8_t)Rnd();
        for (int i = 0; i < 16; i++) token[i] = (uint8_t)Rnd();
    }
    static void Tap(void* c, const MpAddr& from, const MpAddr& to, const void* data, size_t len) {
        RawJoiner* j = (RawJoiner*)c;
        if (!(to == j->me)) return;
        const uint8_t* d = (const uint8_t*)data;
        if (len >= 2 && d[0] == 'G' && d[1] == 'S') {
            if (!j->haveCookie) return;
            std::vector<uint8_t> plain(len);
            size_t pl = 0;
            if (mpseal::Open(j->key, d, len, plain.data(), &pl)) {
                plain.resize(pl);
                j->got.push_back(plain);
            }
            return;
        }
        j->got.push_back(std::vector<uint8_t>(d, d + len));
        if (len == 12 + 8 + 32 + 16 + 4 + 1 + 1 + 1 && d[3] == MP_MSG_COOKIE && !memcmp(d + 12, j->nonce, 8)) {
            memcpy(j->hostPk, d + 20, 32);
            memcpy(j->cookie, d + 52, 16);
            memcpy(&j->sid, d + 68, 4);
            j->hostSlot = d[72];
            j->ctr = ((uint64_t)d[74] << 40) + 1;
            j->haveCookie = mpseal::PairKey(j->sk, j->pk, j->hostPk, j->psk, j->sid, j->key);
            (void)from;
        }
    }
    void hello1(MpLoopHub& hub) {
        uint8_t b[12 + 70] = {0};
        b[0] = 'G'; b[1] = 'M'; b[2] = MP_PROTO_VERSION; b[3] = MP_MSG_HELLO; b[4] = 0xFF;
        uint8_t* p = b + 12;
        // fingerprint is filled by the caller through fp
        memcpy(p, &fp, 4);
        p[4] = (uint8_t)MP_PROTO_VERSION; p[5] = (uint8_t)(MP_PROTO_VERSION >> 8);
        memcpy(p + 6, pk, 32);
        memcpy(p + 38, nonce, 8);
        hub.inject(me, host, b, sizeof b);
    }
    uint32_t fp = 0;
    // the reliable HELLO (seq given) with the fields a test may bend
    struct Hello {
        const char* name = "Raw";
        uint8_t look = 0, mode = 0;
        uint32_t wantSession = 0, consoleId = 0, gid = 0;
        uint16_t proto = MP_PROTO_VERSION;
        uint32_t fp = 0;   // 0: the joiner's
        bool badProof = false;
        uint8_t rawName[MP_NAME_BYTES];
        bool useRaw = false;
    };
    std::vector<uint8_t> hello2(const Hello& h, uint16_t seq) {
        std::vector<uint8_t> b(12, 0);
        b[0] = 'G'; b[1] = 'M'; b[2] = MP_PROTO_VERSION; b[3] = MP_MSG_HELLO; b[4] = 0xFF; b[5] = MP_HDR_RELIABLE;
        b[6] = (uint8_t)seq; b[7] = (uint8_t)(seq >> 8);
        auto put = [&](const void* d, size_t n) { b.insert(b.end(), (const uint8_t*)d, (const uint8_t*)d + n); };
        const uint32_t f = h.fp ? h.fp : fp;
        put(&f, 4);
        put(&h.proto, 2);
        uint8_t nm[MP_NAME_BYTES] = {0};
        if (h.useRaw) memcpy(nm, h.rawName, sizeof nm);
        else snprintf((char*)nm, sizeof nm, "%s", h.name);
        put(nm, sizeof nm);
        put(&h.look, 1);
        put(&h.mode, 1);
        put(&h.wantSession, 4);
        put(&h.consoleId, 4);
        put(&h.gid, 4);
        put(pk, 32);
        put(nonce, 8);
        put(cookie, 16);
        uint8_t pm[29], proof[16], mask[16], tok[16], commit[16];
        memcpy(pm, "hello", 5);
        memcpy(pm + 5, cookie, 16);
        memcpy(pm + 21, nonce, 8);
        mpseal::Mac(proof, 16, key, 32, pm, sizeof pm);
        if (h.badProof) proof[0] ^= 1;
        put(proof, 16);
        memcpy(pm, "tmask", 5);
        mpseal::Mac(mask, 16, key, 32, pm, sizeof pm);
        for (int i = 0; i < 16; i++) tok[i] = (uint8_t)(token[i] ^ mask[i]);
        put(tok, 16);
        mpseal::Mac(commit, 16, 0, 0, token, 16);
        put(commit, 16);
        return b;
    }
    // the whole join up to the HELLO (the caller steps the cores between): true when the host answered with a cookie
    bool join(MpLoopHub& hub, std::vector<MpCore*>& all, const Hello& h, uint16_t seq = 0x4321) {
        hub.setTap(Tap, this);
        hello1(hub);
        for (int i = 0; i < 20 && !haveCookie; i++) Step(all, 5);
        if (!haveCookie) {
            hub.setTap(0, 0);
            return false;
        }
        std::vector<uint8_t> b = hello2(h, seq);
        hub.inject(me, host, b.data(), b.size());
        return true;
    }
    // a packet from this joiner, sealed with the pair key
    void send(MpLoopHub& hub, const void* pkt, size_t len) {
        std::vector<uint8_t> out(len + mpseal::kOverhead);
        const size_t n = mpseal::Seal(key, MP_PROTO_VERSION, 0xFF, ctr++, (const uint8_t*)pkt, len, out.data(), out.size());
        if (n) hub.inject(me, host, out.data(), n);
    }
    void done(MpLoopHub& hub) { hub.setTap(0, 0); }
};
