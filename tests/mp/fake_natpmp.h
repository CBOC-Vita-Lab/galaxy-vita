// Galaxy 3.0 multiplayer (agent natpmp): a fake NAT-PMP / PCP gateway on 127.0.0.1 for host tests (natpmp_test.cpp, portmap_test.cpp): a UDP
// socket in a thread that speaks RFC 6886 and RFC 6887 with switchable behaviour and a request log.
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

static uint64_t NpmpNowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000;
}

static uint16_t R16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
static uint32_t R32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
static void W16(uint8_t* p, unsigned v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
static void W32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

// ---- the fake gateway ---------------------------------------------------------------------------------------------------------------
struct FakeGw {
    // behaviour (set before Start; the ones marked * may change while it runs, under mu)
    bool pmp = true, pcp = false;
    bool silent = false;                 // * answers nothing
    int pmpAddrResult = 0, pmpMapResult = 0, pcpResult = 0;   // * result code of every answer of that kind
    uint32_t extIp = 0xCB007107;         // 203.0.113.7
    uint32_t maxLease = 7200;
    int dropFirst = 0;                   // the first N requests of each kind are not answered (retransmission)
    bool rogue = false;                  // also answers from 127.0.0.2:5351-ish and from another port (valid-looking replies)
    bool rogueOnly = false;              // ... and the real gateway says nothing
    int junkBefore = 0;                  // 1..: before each real reply, a mangled one (see Junk())
    bool takenFirst = false;             // the suggested external port is taken: it gives suggested + 1
    bool pcpRejectSuggested = false;     // PCP: result 11 when a non-zero external port is suggested
    uint32_t zeroLease = 0;              // 1: grants lease 0
    bool zeroExtPort = false;
    int64_t epochBias = 0;               // * added to the epoch seconds
    uint32_t epochFixed = 0;             // * non-zero: every answer says exactly this epoch
    // state
    std::mutex mu;
    std::vector<std::string> reqs;       // "pmp-addr", "pmp-map int=.. ext=.. life=..", "pcp-map ...", "pcp-announce", "pmp-unsupp-pcp"
    std::map<unsigned, unsigned> table;  // external port -> internal port
    int dropped[4] = {0, 0, 0, 0};
    int port = 0;
    int fd = -1, rfd1 = -1, rfd2 = -1;
    pthread_t th;
    std::atomic<bool> stop{false};
    uint64_t startMs = 0;

    bool Start() {
        startMs = NpmpNowMs();
        fd = Bind("127.0.0.1", 0);
        if (fd < 0) return false;
        sockaddr_in a;
        socklen_t l = sizeof a;
        getsockname(fd, (sockaddr*)&a, &l);
        port = ntohs(a.sin_port);
        rfd1 = Bind("127.0.0.2", 0);   // right port is not possible (ours is taken): same address, other port
        rfd2 = Bind("127.0.0.1", 0);   // same address, other port
        return pthread_create(&th, 0, &FakeGw::Main, this) == 0;
    }
    ~FakeGw() { Stop(); }
    void Stop() {
        if (fd < 0) return;
        stop = true;
        pthread_join(th, 0);
        close(fd);
        if (rfd1 >= 0) close(rfd1);
        if (rfd2 >= 0) close(rfd2);
        fd = -1;
    }
    static int Bind(const char* ip, int port) {
        const int s = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, ip, &a.sin_addr);
        if (bind(s, (sockaddr*)&a, sizeof a) != 0) {
            close(s);
            return -1;
        }
        return s;
    }
    static void* Main(void* p) {
        static_cast<FakeGw*>(p)->Loop();
        return 0;
    }
    int Count(const char* prefix) {
        std::lock_guard<std::mutex> g(mu);
        int n = 0;
        for (auto& r : reqs)
            if (r.find(prefix) == 0) ++n;
        return n;
    }
    size_t TableSize() {
        std::lock_guard<std::mutex> g(mu);
        return table.size();
    }

    uint32_t Epoch() {
        if (epochFixed) return epochFixed;
        const int64_t e = int64_t((NpmpNowMs() - startMs) / 1000) + epochBias;
        return e < 0 ? 0 : uint32_t(e);
    }

    void SendTo(int from, const sockaddr_in& to, const uint8_t* d, size_t n) { sendto(from, d, n, 0, (const sockaddr*)&to, sizeof to); }

    // a mangled answer for the junk tests, built from a real one
    void Junk(const sockaddr_in& to, const uint8_t* real, size_t n, int kind) {
        uint8_t b[1500];
        memset(b, 0, sizeof b);
        size_t m = n;
        memcpy(b, real, n < sizeof b ? n : sizeof b);
        switch (kind) {
        case 1: m = n > 10 ? 10 : n - 1; break;                     // truncated
        case 2: m = 1400; break;                                    // oversized (real bytes first, then zeros)
        case 3: b[0] = 1; break;                                    // version 1
        case 4: b[1] = real[0] == 2 ? 0x82 : 130; break;            // unknown opcode (TCP map for NAT-PMP)
        case 5:                                                     // wrong internal port / nonce
            if (real[0] == 0 && real[1] == 129) W16(b + 8, R16(b + 8) + 1);
            else if (real[0] == 0) b[1] = 129;   // an address reply with a map opcode: wrong length
            else b[24] ^= 0xFF;
            break;
        case 6: m = n + 3; break;                                   // not the exact length (PCP: not a multiple of 4)
        default: break;
        }
        SendTo(fd, to, b, m);
    }

    void Loop() {
        while (!stop) {
            pollfd p = {fd, POLLIN, 0};
            if (poll(&p, 1, 5) <= 0) continue;
            uint8_t in[1500];
            sockaddr_in from;
            socklen_t fl = sizeof from;
            const ssize_t n = recvfrom(fd, in, sizeof in, 0, (sockaddr*)&from, &fl);
            if (n <= 0) continue;
            std::lock_guard<std::mutex> g(mu);
            Handle(in, size_t(n), from);
        }
    }

    // ----
    void Answer(const sockaddr_in& to, int kind, const uint8_t* d, size_t n) {
        if (dropped[kind] < dropFirst) {
            ++dropped[kind];
            return;
        }
        if (rogue || rogueOnly) {
            // the same answer from another address and from another port (the client must ignore both)
            if (rfd1 >= 0) SendTo(rfd1, to, d, n);
            SendTo(rfd2, to, d, n);
        }
        if (rogueOnly || silent) return;
        if (junkBefore) Junk(to, d, n, junkBefore);
        SendTo(fd, to, d, n);
    }

    void Handle(const uint8_t* d, size_t n, const sockaddr_in& from) {
        const uint32_t srcIp = ntohl(from.sin_addr.s_addr);
        char t[160];
        if (n >= 2 && d[0] == 0 && d[1] == 0 && n == 2) {   // NAT-PMP external address
            reqs.push_back("pmp-addr");
            if (!pmp) return;
            uint8_t r[12];
            r[0] = 0; r[1] = 128;
            W16(r + 2, pmpAddrResult);
            W32(r + 4, Epoch());
            W32(r + 8, pmpAddrResult ? 0 : extIp);
            Answer(from, 0, r, sizeof r);
            return;
        }
        if (n == 12 && d[0] == 0 && d[1] == 1) {   // NAT-PMP map UDP
            const unsigned ip = R16(d + 4), ep = R16(d + 6);
            const uint32_t life = R32(d + 8);
            snprintf(t, sizeof t, "pmp-map int=%u ext=%u life=%u", ip, ep, life);
            reqs.push_back(t);
            if (!pmp) return;
            uint8_t r[16];
            r[0] = 0; r[1] = 129;
            W16(r + 2, pmpMapResult);
            W32(r + 4, Epoch());
            W16(r + 8, ip);
            unsigned ext = 0, grant = 0;
            if (life == 0) {
                for (auto it = table.begin(); it != table.end();) {
                    if (it->second == ip) it = table.erase(it);
                    else ++it;
                }
            } else if (pmpMapResult == 0) {
                ext = ep ? ep : ip;
                if (takenFirst && ep) ext = ep + 1;
                if (zeroExtPort) ext = 0;
                grant = life < maxLease ? life : maxLease;
                if (zeroLease) grant = 0;
                table[ext] = ip;
            }
            W16(r + 10, pmpMapResult ? 0 : ext);
            W32(r + 12, pmpMapResult ? 0 : grant);
            Answer(from, 1, r, sizeof r);
            return;
        }
        if (n >= 24 && d[0] == 2) {   // PCP
            const unsigned op = d[1] & 0x7F;
            if (!pcp) {
                reqs.push_back("pmp-unsupp-pcp");
                if (!pmp) return;
                uint8_t r[8] = {0, uint8_t(128 + op), 0, 1, 0, 0, 0, 0};   // RFC 6886 section 3.5: version 0, opcode + 128, result 1
                W32(r + 4, Epoch());
                Answer(from, 2, r, sizeof r);
                return;
            }
            uint8_t r[60];
            memset(r, 0, sizeof r);
            r[0] = 2;
            r[1] = uint8_t(0x80 | op);
            W32(r + 8, Epoch());
            uint32_t cip = 0;
            const bool mapped = d[18] == 0xFF && d[19] == 0xFF;
            if (mapped) cip = R32(d + 20);
            if (op == 0) {
                reqs.push_back("pcp-announce");
                r[3] = uint8_t(pcpResult);
                Answer(from, 3, r, 24);
                return;
            }
            if (op != 1 || n != 60) return;
            const unsigned ip = R16(d + 40), ep = R16(d + 42);
            const uint32_t life = R32(d + 4);
            snprintf(t, sizeof t, "pcp-map int=%u ext=%u life=%u", ip, ep, life);
            reqs.push_back(t);
            memcpy(r + 24, d + 24, 12);   // nonce
            r[36] = d[36];
            W16(r + 40, ip);
            int res = pcpResult;
            if (!res && cip != srcIp) res = 12;
            if (!res && pcpRejectSuggested && ep) res = 11;
            r[3] = uint8_t(res);
            unsigned ext = 0, grant = 0;
            if (life == 0) {
                for (auto it = table.begin(); it != table.end();) {
                    if (it->second == ip) it = table.erase(it);
                    else ++it;
                }
            } else if (res == 0) {
                ext = ep ? ep : ip;
                if (takenFirst && ep) ext = ep + 1;
                grant = life < maxLease ? life : maxLease;
                if (zeroLease) grant = 0;
                if (zeroExtPort) ext = 0;
                table[ext] = ip;
            }
            W32(r + 4, res ? 30 : grant);
            W16(r + 42, ext);
            r[44 + 10] = r[44 + 11] = 0xFF;
            W32(r + 44 + 12, extIp);
            Answer(from, 3, r, sizeof r);
            return;
        }
        reqs.push_back("junk");
    }
};

