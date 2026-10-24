#pragma once

#ifndef GXV_RECIPE_SEED_BATCH
#define GXV_RECIPE_SEED_BATCH 0
#endif

#if GXV_RECIPE_SEED_BATCH
#define GXV_RECIPE_SEED_API 1
#include "vgx_shadergen.h"
#include "vgx_variant_set.h"
#include <array>
#include <cerrno>
#include <string>
#include <unordered_set>
#include <vector>

namespace vgx {
void SetShaderRecipeSeedFile(const char* path);

namespace recipe {
constexpr size_t kSeedSize = 36356;
constexpr uint8_t kSeedDigest[32] = {
    0xc7, 0xb2, 0xea, 0xd0, 0x78, 0xe4, 0x2d, 0x79, 0x4c, 0xb2, 0x57, 0x4f, 0x16, 0x7a, 0x09, 0x17,
    0x9b, 0xaa, 0x8a, 0x45, 0x15, 0xf4, 0x49, 0x8a, 0xdb, 0x5d, 0x8e, 0x73, 0x62, 0xca, 0x00, 0xe6
};
constexpr size_t kMaxListSize = 4u * 1024u * 1024u;
constexpr uint32_t kMaxKeys = 4096;
constexpr uint32_t kNoFileId = ~0u;

inline bool Eligible() {
    const char* value = std::getenv("GXV_RECIPE_SEED");
    return value && std::strcmp(value, "1") == 0 && !std::getenv("GXV_SHADERLIST");
}

inline uint32_t Rotate(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
inline std::array<uint8_t, 32> Digest(const uint8_t* bytes, size_t size) {
    static constexpr uint32_t round[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const size_t blocks = size / 64 + (size % 64 < 56 ? 1 : 2);
    const uint64_t bitSize = static_cast<uint64_t>(size) * 8;
    for (size_t block = 0; block < blocks; ++block) {
        uint32_t w[64] = {};
        for (size_t i = 0; i < 64; ++i) {
            const size_t pos = block * 64 + i;
            uint8_t b = pos < size ? bytes[pos] : (pos == size ? 0x80 : 0);
            if (block == blocks - 1 && i >= 56) b = static_cast<uint8_t>(bitSize >> ((63 - i) * 8));
            w[i / 4] |= static_cast<uint32_t>(b) << ((3 - (i % 4)) * 8);
        }
        for (unsigned i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotate(w[i - 15], 7) ^ Rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotate(w[i - 2], 17) ^ Rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], z = h[7];
        for (unsigned i = 0; i < 64; ++i) {
            const uint32_t s1 = Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25);
            const uint32_t t1 = z + s1 + ((e & f) ^ (~e & g)) + round[i] + w[i];
            const uint32_t s0 = Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22);
            const uint32_t t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
            z = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += z;
    }
    std::array<uint8_t, 32> out{};
    for (unsigned i = 0; i < 32; ++i) out[i] = static_cast<uint8_t>(h[i / 4] >> ((3 - (i % 4)) * 8));
    return out;
}

struct Header { char magic[8]; uint32_t vsSize, fsSize, version, reserved; };
struct Record { uint32_t type, v[4]; };
struct List { std::vector<VsKey> vs; std::vector<FsKey> fs; std::vector<Record> records; };

inline bool Safe(const VsKey& k) {
    if (k.numTexGens > 8 || k.numChans > 2 || k.perVtxPos > 2) return false;
    for (const auto& c : k.chan)
        if (c.enable > 1 || c.matSrcVtx > 1 || c.ambSrcVtx > 1 || c.diffuse > 2 || c.attn > 2) return false;
    for (const auto& t : k.tg)
        if (t.type > 3 || t.src > 31 || t.proj > 1 || t.inputAbc1 > 1 || t.post > 2 || t.perVtxMtx > 1) return false;
    return true;
}
inline bool Safe(const FsKey& k) {
    if (k.numStages < 1 || k.numStages > 16 || k.numTexGens > 8 || k.numIndStages > 4 || k.alphaTestOn > 1 || k.dstAlpha > 1) return false;
    for (const auto& s : k.stage)
        if (s.texEnable > 1 || s.texMap > 7 || s.texCoord > 7 || s.rasChan > 7 || s.kcsel > 31 || s.kasel > 31) return false;
    for (const auto& i : k.ind) if (i.texMap > 7 || i.texCoord > 7) return false;
    return true;
}

inline uint64_t Variant(uint32_t vs, uint32_t fs, uint32_t layout, uint32_t blend) {
    return (static_cast<uint64_t>((vs << 16) | fs) << 32) | (layout << 12) | blend;
}
inline bool Parse(const std::vector<uint8_t>& bytes, List& out) {
    if (bytes.size() < sizeof(Header) || bytes.size() > kMaxListSize) return false;
    Header h;
    std::memcpy(&h, bytes.data(), sizeof h);
    if (std::memcmp(h.magic, "VGXSHL01", 8) || h.vsSize != sizeof(VsKey) || h.fsSize != sizeof(FsKey) ||
        h.version != kShaderGenVersion || h.reserved != 0) return false;
    List parsed;
    std::unordered_set<std::string> vsKeys, fsKeys;
    std::unordered_set<uint32_t> pairs;
    VariantSet variants;
    size_t offset = sizeof h;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 4 || parsed.records.size() >= 32768) return false;
        Record r{};
        std::memcpy(&r.type, bytes.data() + offset, 4);
        offset += 4;
        const size_t size = r.type == 1 ? sizeof(VsKey) : r.type == 2 ? sizeof(FsKey) : r.type == 3 ? 8 : r.type == 4 ? 16 : 0;
        if (!size || bytes.size() - offset < size) return false;
        const uint8_t* p = bytes.data() + offset;
        if (r.type == 1) {
            VsKey k;
            std::memcpy(&k, p, sizeof k);
            if (!Safe(k) || parsed.vs.size() >= kMaxKeys || !vsKeys.emplace(reinterpret_cast<const char*>(p), size).second) return false;
            r.v[0] = static_cast<uint32_t>(parsed.vs.size());
            parsed.vs.push_back(k);
        } else if (r.type == 2) {
            FsKey k;
            std::memcpy(&k, p, sizeof k);
            if (!Safe(k) || parsed.fs.size() >= kMaxKeys || !fsKeys.emplace(reinterpret_cast<const char*>(p), size).second) return false;
            r.v[0] = static_cast<uint32_t>(parsed.fs.size());
            parsed.fs.push_back(k);
        } else {
            std::memcpy(r.v, p, size);
            if (r.v[0] >= parsed.vs.size() || r.v[1] >= parsed.fs.size()) return false;
            if (r.type == 3) {
                if (!pairs.insert((r.v[0] << 16) | r.v[1]).second) return false;
            } else if (r.v[2] > 0xFFF || r.v[3] > 0xFFF || !variants.insert(Variant(r.v[0], r.v[1], r.v[2], r.v[3])).second) return false;
        }
        parsed.records.push_back(r);
        offset += size;
    }
    out = std::move(parsed);
    return true;
}

inline bool Read(const char* path, size_t limit, std::vector<uint8_t>& bytes, bool* absent = nullptr) {
    if (absent) *absent = false;
    FILE* f = std::fopen(path, "rb");
    if (!f) { if (absent && errno == ENOENT) { *absent = true; return true; } return false; }
    bool ok = std::fseek(f, 0, SEEK_END) == 0;
    const long n = ok ? std::ftell(f) : -1;
    ok = n >= 0 && static_cast<unsigned long>(n) <= limit && std::fseek(f, 0, SEEK_SET) == 0;
    std::vector<uint8_t> data;
    if (ok) {
        data.resize(static_cast<size_t>(n));
        ok = (data.empty() || std::fread(data.data(), 1, data.size(), f) == data.size()) && std::fgetc(f) == EOF && !std::ferror(f);
    }
    if (std::fclose(f) != 0) ok = false;
    if (ok) bytes = std::move(data);
    return ok;
}
inline bool ReadSeed(const char* path, List& list) {
    std::vector<uint8_t> bytes;
    if (!Read(path, kSeedSize, bytes) || bytes.size() != kSeedSize) return false;
    const auto digest = Digest(bytes.data(), bytes.size());
    return std::memcmp(digest.data(), kSeedDigest, sizeof kSeedDigest) == 0 && Parse(bytes, list);
}
} // namespace recipe
} // namespace vgx
#endif
