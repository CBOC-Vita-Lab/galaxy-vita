#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace vgx::vita {
class ShaderPack {
public:
    static constexpr size_t kHeaderBytes = 40, kEntryBytes = 24;
    static constexpr size_t kMaxBytes = 1u << 20, kMaxRecordBytes = 256u << 10;
    static constexpr uint32_t kMaxEntries = 4096;
    ShaderPack() = default;
    ShaderPack(const ShaderPack&) = delete;
    ShaderPack& operator=(const ShaderPack&) = delete;
    ~ShaderPack() { std::free(bytes_); }

    // Startup owner only, before any GL worker starts. Lookups are then immutable.
    bool LoadFile(const char* filename, const char* root, uint64_t abi) {
        std::free(bytes_); bytes_ = nullptr; size_ = count_ = 0; root_[0] = 0;
        const size_t rootSize = Length(root, sizeof root_);
        if (!filename || !rootSize || rootSize > 224 || root[rootSize - 1] == '/') return false;
        FILE* file = std::fopen(filename, "rb");
        if (!file) return false;
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
    bool Find(const char* path, const void** data, size_t* size) const {
        if (data) *data = nullptr;
        if (size) *size = 0;
        if (!bytes_ || !data || !size) return false;
        uint32_t stage; uint64_t hash;
        if (!ParsePath(root_, rootSize_, path, stage, hash)) return false;
        uint32_t low = 0, high = count_;
        while (low < high) {
            const uint32_t mid = low + (high - low) / 2;
            const uint8_t* e = bytes_ + kHeaderBytes + mid * kEntryBytes;
            const uint32_t kind = U32(e); const uint64_t key = U64(e + 8);
            if (kind < stage || (kind == stage && key < hash)) low = mid + 1;
            else high = mid;
        }
        if (low == count_) return false;
        const uint8_t* e = bytes_ + kHeaderBytes + low * kEntryBytes;
        if (U32(e) != stage || U64(e + 8) != hash) return false;
        *data = bytes_ + static_cast<size_t>(U64(e + 16)); *size = U32(e + 4);
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
