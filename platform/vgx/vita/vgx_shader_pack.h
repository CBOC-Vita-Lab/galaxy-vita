#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include "port/bootio.h"
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace vgx::vita {
class ShaderPack {
public:
    static constexpr size_t kHeaderBytes = 40, kEntryBytes = 24;
    static constexpr size_t kMaxBytes = 8u << 20, kMaxRecordBytes = 256u << 10;
    static constexpr uint32_t kMaxEntries = 8192;   // (pack25: was 4 MiB / 4096; the whole C182 seed, 5,269+ binaries, plus a harvest; = tools/c182_compact_seed.py)
    ShaderPack() = default;
    ShaderPack(const ShaderPack&) = delete;
    ShaderPack& operator=(const ShaderPack&) = delete;
    ~ShaderPack() { std::free(bytes_); }

    // Startup owner only, before any GL worker starts. Lookups are then immutable.
    bool LoadFile(const char* filename, const char* root, uint64_t abi, bool retryMissing = false) {
        std::free(bytes_); bytes_ = nullptr; size_ = count_ = 0; root_[0] = 0;
        const size_t rootSize = Length(root, sizeof root_);
        if (!filename || !rootSize || rootSize > 224 || root[rootSize - 1] == '/') return false;
        // (bootio: the package's own pack must be there, so its "no such file" is retried too; the card's copy is normally absent: one try)
        bootio::Options patience;
        if (!retryMissing) patience.missingMs = 0;
        FILE* file = bootio::FopenRetry(filename, "rb", "shader pack", patience);
        if (!file) return false;
        // Bypass small default stdio buffer (1 KiB in newer VitaSDK) so the whole-pack read issues one syscall.
        std::setvbuf(file, nullptr, _IONBF, 0);
        const bool end = std::fseek(file, 0, SEEK_END) == 0;
        const long length = end ? std::ftell(file) : -1;
        if (length < static_cast<long>(kHeaderBytes) || static_cast<uint64_t>(length) > kMaxBytes ||
            std::fseek(file, 0, SEEK_SET) != 0) { std::fclose(file); return false; }
        const size_t size = static_cast<size_t>(length);
        std::unique_ptr<uint8_t, Free> bytes(static_cast<uint8_t*>(std::malloc(size)));
        if (!bytes) { std::fclose(file); return false; }
        size_t read = 0;
        while (read < size) {
            const size_t got = std::fread(bytes.get() + read, 1, size - read, file);
            if (!got) break;
            read += got;
        }
        uint8_t extra;
        const bool exact = read == size && std::fread(&extra, 1, 1, file) == 0 && !std::ferror(file);
        const bool closed = std::fclose(file) == 0;
        if (!exact || !closed || !Validate(bytes.get(), size, abi)) return false;
        bytes_ = bytes.release(); size_ = size; count_ = U32(bytes_ + 12);
        std::memcpy(root_, root, rootSize + 1);
        rootSize_ = rootSize;
        return true;
    }
    bool Find(const char* path, const void** data, size_t* size) const { return FindIndex(path, data, size) >= 0; }
    // cook24: the same lookup, answering the record's number (0..Entries()-1, in the pack's sorted order) or -1.
    int FindIndex(const char* path, const void** data, size_t* size) const {
        if (data) *data = nullptr;
        if (size) *size = 0;
        if (!bytes_ || !data || !size) return -1;
        uint32_t stage; uint64_t hash;
        if (!ParsePath(root_, rootSize_, path, stage, hash)) return -1;
        uint32_t low = 0, high = count_;
        while (low < high) {
            const uint32_t mid = low + (high - low) / 2;
            const uint8_t* e = bytes_ + kHeaderBytes + mid * kEntryBytes;
            const uint32_t kind = U32(e); const uint64_t key = U64(e + 8);
            if (kind < stage || (kind == stage && key < hash)) low = mid + 1;
            else high = mid;
        }
        if (low == count_) return -1;
        const uint8_t* e = bytes_ + kHeaderBytes + low * kEntryBytes;
        if (U32(e) != stage || U64(e + 8) != hash) return -1;
        *data = bytes_ + static_cast<size_t>(U64(e + 16)); *size = U32(e + 4);
        return static_cast<int>(low);
    }
    // cook24: record `i` of a loaded pack (stage 1 = vertex, 2 = fragment).
    bool At(uint32_t i, uint32_t& stage, uint64_t& hash, const uint8_t*& data, uint32_t& length) const {
        if (!bytes_ || i >= count_) return false;
        const uint8_t* e = bytes_ + kHeaderBytes + i * kEntryBytes;
        stage = U32(e); length = U32(e + 4); hash = U64(e + 8); data = bytes_ + static_cast<size_t>(U64(e + 16));
        return true;
    }
    // cook24 (the user pack): takes a malloc'ed image already read (ownership passes only when true is returned).
    bool Adopt(uint8_t* image, size_t size, const char* root, uint64_t abi) {
        const size_t rootSize = Length(root, sizeof root_);
        if (!image || size < kHeaderBytes || size > kMaxBytes || !rootSize || rootSize > 224 || root[rootSize - 1] == '/') return false;
        if (!Validate(image, size, abi)) return false;
        std::free(bytes_);
        bytes_ = image; size_ = size; count_ = U32(bytes_ + 12);
        std::memcpy(root_, root, rootSize + 1);
        rootSize_ = rootSize;
        return true;
    }
    static bool ValidImage(const uint8_t* b, size_t size, uint64_t abi) { return size >= kHeaderBytes && size <= kMaxBytes && Validate(b, size, abi); }
    static bool ValidRecord(const uint8_t* b, uint32_t length, uint32_t stage) {
        return (stage == 1 || stage == 2) && length >= 8 && length <= kMaxRecordBytes && StageImage(b, length, stage);
    }
    struct Record { uint32_t stage; uint64_t hash; const uint8_t* data; uint32_t length; };
    // cook24: the image of `n` records, which must be sorted by (stage, hash) and distinct. False when it would exceed the limits.
    static bool Build(const Record* r, size_t n, uint64_t abi, std::vector<uint8_t>& out) {
        out.clear();
        if (!n || n > kMaxEntries) return false;
        uint64_t total = kHeaderBytes + n * kEntryBytes;
        for (size_t i = 0; i < n; ++i) {
            if (i && (r[i].stage < r[i - 1].stage || (r[i].stage == r[i - 1].stage && r[i].hash <= r[i - 1].hash))) return false;
            total += (uint64_t(r[i].length) + 3u) & ~uint64_t(3);
        }
        if (total > kMaxBytes) return false;
        out.assign(static_cast<size_t>(total), 0);
        auto p32 = [&](size_t at, uint32_t v) { for (int k = 0; k < 4; ++k) out[at + k] = uint8_t(v >> (8 * k)); };
        auto p64 = [&](size_t at, uint64_t v) { p32(at, uint32_t(v)); p32(at + 4, uint32_t(v >> 32)); };
        std::memcpy(out.data(), "VGXRAM01", 8);
        p32(8, 1); p32(12, static_cast<uint32_t>(n)); p64(16, total); p64(24, abi);
        uint64_t next = kHeaderBytes + n * kEntryBytes;
        for (size_t i = 0; i < n; ++i) {
            const size_t e = kHeaderBytes + i * kEntryBytes;
            p32(e, r[i].stage); p32(e + 4, r[i].length); p64(e + 8, r[i].hash); p64(e + 16, next);
            std::memcpy(out.data() + next, r[i].data, r[i].length);
            next += (uint64_t(r[i].length) + 3u) & ~uint64_t(3);
        }
        p64(32, XXH3_64bits(out.data() + kHeaderBytes, out.size() - kHeaderBytes));
        return true;
    }
    size_t Bytes() const { return size_; }
    uint32_t Entries() const { return count_; }

    static bool ParsePath(const char* root, size_t rootSize, const char* path, uint32_t& stage, uint64_t& hash) {
        const size_t length = Length(path, 320);
        if (!root || !rootSize || length < rootSize + 11 || length > rootSize + 26 ||
            std::memcmp(path, root, rootSize) != 0) return false;
        const char* s = path + rootSize;
        if (s[0] != '/' || (s[1] != 'v' && s[1] != 'f') || s[2] != '/' || s[5] != '/') return false;
        const int a = Hex(s[3]), b = Hex(s[4]);
        const size_t digits = length - rootSize - 10;
        if (a < 0 || b < 0 || (digits > 1 && s[6] == '0') || std::memcmp(s + 6 + digits, ".gxp", 4) != 0) return false;
        hash = 0;
        for (size_t i = 0; i < digits; ++i) {
            const int n = Hex(s[6 + i]); if (n < 0) return false;
            hash = (hash << 4) | static_cast<unsigned>(n);
        }
        stage = s[1] == 'v' ? 1u : 2u;
        return static_cast<unsigned>(hash >> 56) == static_cast<unsigned>(a * 16 + b);
    }

private:
    struct Free { void operator()(void* p) const { std::free(p); } };
    uint8_t* bytes_ = nullptr;
    size_t size_ = 0, rootSize_ = 0;
    uint32_t count_ = 0;
    char root_[256] = {};
    static size_t Length(const char* s, size_t max) {
        if (!s) return 0;
        size_t n = 0; while (n < max && s[n]) ++n; return n;
    }
    static int Hex(char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    }
    static uint32_t U32(const uint8_t* b) {
        return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    }
    static uint64_t U64(const uint8_t* b) { return uint64_t(U32(b)) | uint64_t(U32(b + 4)) << 32; }
    static bool StageImage(const uint8_t* b, uint32_t length, uint32_t stage) {
        const uint64_t matrices = U32(b), prefix = 4u + matrices * 4u;
        if (prefix > length || length - prefix < 156) return false;
        const uint8_t* gxp = b + prefix;
        const uint32_t size = U32(gxp + 8);
        // Public GXP header layout: magic, size at +8, parameter count/+36 and
        // relative parameter-table offset/+40. Preserve native end padding (0..3).
        if (std::memcmp(gxp, "GXP", 4) != 0 || size < 156 ||
            ((uint64_t(size) + 3u) & ~uint64_t(3)) != length - prefix ||
            (U32(gxp + 20) & 1u) != stage - 1u) return false;
        const uint64_t count = U32(gxp + 36), base = 40u + uint64_t(U32(gxp + 40));
        if (count && ((base & 3u) || base < 156 || base > size || count * 16u > size - base)) return false;
        for (uint64_t i = 0; i < matrices; ++i) if (U32(b + 4u + i * 4u) >= count) return false;
        return true;
    }
    static bool Validate(const uint8_t* b, size_t size, uint64_t abi) {
        if (std::memcmp(b, "VGXRAM01", 8) != 0 || U32(b + 8) != 1 || U64(b + 16) != size || U64(b + 24) != abi) return false;
        const uint32_t count = U32(b + 12);
        if (!count || count > kMaxEntries || kHeaderBytes + count * kEntryBytes > size ||
            U64(b + 32) != XXH3_64bits(b + kHeaderBytes, size - kHeaderBytes)) return false;
        uint64_t next = kHeaderBytes + count * kEntryBytes, previousHash = 0;
        uint32_t previousStage = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* e = b + kHeaderBytes + i * kEntryBytes;
            const uint32_t stage = U32(e), length = U32(e + 4);
            const uint64_t hash = U64(e + 8), offset = U64(e + 16);
            if ((stage != 1 && stage != 2) || length < 8 || length > kMaxRecordBytes || offset != next ||
                (offset & 3u) || offset > size || length > size - offset ||
                (i && (stage < previousStage || (stage == previousStage && hash <= previousHash)))) return false;
            // Basic integrity, not full validation of GPU code or reflection strings.
            if (!StageImage(b + offset, length, stage)) return false;
            const uint64_t aligned = (uint64_t(length) + 3u) & ~uint64_t(3);
            if (aligned > size - offset) return false;
            for (uint64_t p = length; p < aligned; ++p) if (b[offset + p] != 0) return false;
            next = offset + aligned; previousStage = stage; previousHash = hash;
        }
        return next == size;
    }
};
}
