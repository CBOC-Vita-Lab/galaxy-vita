#ifndef SHADER_PACK_HEADER
#define SHADER_PACK_HEADER "../platform/vgx/vita/vgx_shader_pack.h"
#endif
#include SHADER_PACK_HEADER
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
using vgx::vita::ShaderPack;
std::atomic<bool> failAlloc{false}, failRead{false}, tinyReads{false}, failClose{false};
std::atomic<unsigned> failuresConsumed{0};
constexpr uint64_t abi = 0x128fdc77054192b3ull;
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void U32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (unsigned i = 0; i < 4; ++i) v.at(at + i) = static_cast<uint8_t>(x >> (8 * i));
}
void U64(std::vector<uint8_t>& v, size_t at, uint64_t x) {
    for (unsigned i = 0; i < 8; ++i) v.at(at + i) = static_cast<uint8_t>(x >> (8 * i));
}
void Seal(std::vector<uint8_t>& v) { U64(v, 32, XXH3_64bits(v.data() + 40, v.size() - 40)); }
std::vector<uint8_t> Pack() {
    // Independent wire encoder, not the production builder or parser.
    std::vector<uint8_t> v(40 + 3 * 24 + 3 * 180, 0);
    std::memcpy(v.data(), "VGXRAM01", 8); U32(v, 8, 1); U32(v, 12, 3);
    U64(v, 16, v.size()); U64(v, 24, abi);
    const uint64_t hashes[] = {0, 0x78af5f94892f3950ull, 0xf9834ce197a67231ull};
    for (unsigned i = 0; i < 3; ++i) {
        const size_t e = 40 + i * 24, p = 112 + i * 180, gxp = p + 4;
        U32(v, e, i == 2 ? 2 : 1); U32(v, e + 4, 180); U64(v, e + 8, hashes[i]); U64(v, e + 16, p);
        std::memcpy(v.data() + gxp, "GXP", 4); U32(v, gxp + 8, 175); U32(v, gxp + 20, i == 2 ? 1 : 0);
        U32(v, gxp + 36, 1); U32(v, gxp + 40, 116);
        for (unsigned j = 156; j < 176; ++j) v[gxp + j] = static_cast<uint8_t>(j * 7 + i);
    }
    Seal(v); return v;
}
void Put(const std::string& file, const std::vector<uint8_t>& v) {
    FILE* f = std::fopen(file.c_str(), "wb"); Require(f != nullptr, "fixture opens output");
    Require(std::fwrite(v.data(), 1, v.size(), f) == v.size() && std::fclose(f) == 0, "fixture writes a complete pack");
}
void Miss(const ShaderPack& p, const char* path) {
    const void* data = reinterpret_cast<const void*>(uintptr_t(1)); size_t size = 99;
    Require(!p.Find(path, &data, &size) && data == nullptr && size == 0, "miss resets borrowed outputs without allocation or I/O");
}
}
extern "C" {
void* __real_malloc(size_t);
void* __wrap_malloc(size_t n) {
    if (failAlloc && n == 652) { failAlloc = false; ++failuresConsumed; return nullptr; }
    return __real_malloc(n);
}
size_t __real_fread(void*, size_t, size_t, FILE*);
size_t __wrap_fread(void* p, size_t size, size_t count, FILE* f) {
    if (failRead) { failRead = false; ++failuresConsumed; return 0; }
    if (tinyReads && size == 1) count = std::min(count, size_t(3));
    return __real_fread(p, size, count, f);
}
// GCC/glibc fortifies the bulk read, but not the one-byte EOF probe.
size_t __real___fread_chk(void*, size_t, size_t, size_t, FILE*);
size_t __wrap___fread_chk(void* p, size_t bound, size_t size, size_t count, FILE* f) {
    if (failRead) { failRead = false; ++failuresConsumed; return 0; }
    if (tinyReads && size == 1) count = std::min(count, size_t(3));
    return __real___fread_chk(p, bound, size, count, f);
}
int __real_fclose(FILE*);
int __wrap_fclose(FILE* f) {
    const int result = __real_fclose(f);
    if (failClose) { failClose = false; ++failuresConsumed; return EOF; }
    return result;
}
}

int main() {
    const std::string file = "/tmp/vgx-shader-pack-unit.bin";
    ShaderPack p;
    Miss(p, "/root/v/78/78AF5F94892F3950.gxp");
    const auto original = Pack(); Put(file, original);
    Require(p.LoadFile(file.c_str(), "/root", abi), "valid bounded shader pack loads before rendering");
    Require(p.Bytes() == original.size() && p.Entries() == 3, "retained memory and record count are exact");
    const char* names[] = {"/root/v/00/0.gxp", "/root/v/78/78AF5F94892F3950.gxp", "/root/f/F9/F9834CE197A67231.gxp"};
    for (unsigned i = 0; i < 3; ++i) {
        const void* data = nullptr; size_t size = 0;
        Require(p.Find(names[i], &data, &size) && size == 180 && std::memcmp(data, original.data() + 112 + 180 * i, 180) == 0,
                "lookup borrows the entire exact serialized stage image, not just program bytes");
        Require((reinterpret_cast<uintptr_t>(data) & 3u) == 0, "matrix metadata has native word alignment");
    }
    const char* invalid[] = {"/root", "/evil/v/78/78AF5F94892F3950.gxp", "/rootx/v/78/78AF5F94892F3950.gxp", "/root/v/79/78AF5F94892F3950.gxp",
        "/root/v/78/78af5f94892f3950.gxp", "/root/v/78/078AF5F94892F3950.gxp", "/root/v/78/78AF5F94892F3950.gxpX",
        "/root/x/78/78AF5F94892F3950.gxp", "/root/f/78/78AF5F94892F3950.gxp", "/root/v/78/1.gxp",
        "/root/v/7/78AF5F94892F3950.gxp", "/root/../v/78/78AF5F94892F3950.gxp", "/root//v/78/78AF5F94892F3950.gxp"};
    for (const auto* name : invalid) Miss(p, name);
    Require(!p.Find(names[0], nullptr, nullptr), "invalid API outputs are refused");
    std::atomic<bool> concurrentOk{true};
    std::thread workers[4];
    for (auto& worker : workers) worker = std::thread([&] {
        for (unsigned i = 0; i < 1000; ++i) {
            const void* data = nullptr; size_t size = 0;
            if (!p.Find(names[i % 3], &data, &size) || size != 180) concurrentOk = false;
        }
    });
    for (auto& worker : workers) worker.join();
    Require(concurrentOk, "immutable lookup is safe across readers after startup publication");

    unsigned rejected = 0;
    for (unsigned kind = 0; kind < 29; ++kind) {
        auto bad = original;
        switch (kind) {
        case 0: bad[0] ^= 1; break;
        case 1: U32(bad, 8, 2); break;
        case 2: U32(bad, 12, 4097); break;
        case 3: U32(bad, 12, 0); break;
        case 4: U64(bad, 16, bad.size() + 1); break;
        case 5: U64(bad, 24, abi ^ 1); break;
        case 6: bad.back() ^= 1; break;
        case 7: U32(bad, 40, 3); Seal(bad); break;
        case 8: U32(bad, 44, 3); Seal(bad); break;
        case 9: U32(bad, 44, 256 * 1024 + 1); Seal(bad); break;
        case 10: U64(bad, 56, uint64_t(-4)); Seal(bad); break;
        case 11: U64(bad, 56, 111); Seal(bad); break;
        case 12: U64(bad, 80, 112); Seal(bad); break;
        case 13: U64(bad, 72, 0); Seal(bad); break;
        case 14: U32(bad, 292, 45); Seal(bad); break;
        case 15: U32(bad, 112, uint32_t(-1)); Seal(bad); break;
        case 16: bad.resize(39); break;
        case 17: bad.resize(ShaderPack::kMaxBytes + 1); break;
        case 18: bad.push_back(0); U64(bad, 16, bad.size()); Seal(bad); break;
        case 19: U32(bad, 44, 24); Seal(bad); break;
        case 20: bad[116] ^= 1; Seal(bad); break;
        case 21: U32(bad, 124, 177); Seal(bad); break;
        case 22: U32(bad, 124, 172); Seal(bad); break;
        case 23: U32(bad, 124, 155); Seal(bad); break;
        case 24: U32(bad, 156, uint32_t(-1)); Seal(bad); break;
        case 25: U32(bad, 152, uint32_t(-1)); Seal(bad); break;
        case 26:
            std::memmove(bad.data() + 120, bad.data() + 116, 172);
            U32(bad, 112, 1); U32(bad, 116, 1); U32(bad, 128, 172); Seal(bad); break;
        case 27: U32(bad, 136, 1); Seal(bad); break;
        case 28: U32(bad, 156, 115); Seal(bad); break;
        }
        Put(file, bad);
        Require(!p.LoadFile(file.c_str(), "/root", abi) && p.Entries() == 0 && p.Bytes() == 0,
                "malformed pack fails closed and cannot retain a stale prior generation");
        Miss(p, names[1]); ++rejected;
    }
    Put(file, original);
    Require(!p.LoadFile(file.c_str(), "/root/", abi), "root with trailing separator is refused");
    Require(!p.LoadFile(file.c_str(), std::string(256, 'x').c_str(), abi), "overlong root is refused");
    Require(!p.LoadFile("/tmp/no-such-shader-pack-283616", "/root", abi), "missing pack leaves ordinary cache available");
    failAlloc = true;
    Require(!p.LoadFile(file.c_str(), "/root", abi) && !failAlloc, "pack allocation failure is retryable and publishes nothing");
    failRead = true;
    Require(!p.LoadFile(file.c_str(), "/root", abi) && !failRead, "short read failure publishes no partial pack");
    failClose = true;
    Require(!p.LoadFile(file.c_str(), "/root", abi) && !failClose, "close error rejects the preload without leaking its handle");
    tinyReads = true;
    Require(p.LoadFile(file.c_str(), "/root", abi), "valid partial reads are accumulated exactly");
    tinyReads = false;
    Require(failuresConsumed == 3, "all three independent fault injections actually ran");
    Require(p.LoadFile(file.c_str(), "/other-root", abi), "cache reconfiguration is permitted only by startup owner");
    Miss(p, names[1]);
    const void* data = nullptr; size_t size = 0;
    Require(p.Find("/other-root/v/78/78AF5F94892F3950.gxp", &data, &size), "lookup never accepts bytes under the previous root");
    std::remove(file.c_str());
    std::printf("PASS: shader pack exact bytes, 29 malformed inputs, path grammar, 4000 concurrent lookups, OOM/read/close fallback (%u rejects; not GPU/FPS evidence)\n", rejected);
}
