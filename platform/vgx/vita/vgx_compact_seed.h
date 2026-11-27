#pragma once
#include "vgx_shader_pack.h"
#include <mutex>

namespace vgx::vita {
// Read-only package backing for loose .gxp files. The hot RAM pack still goes
// first. Hold only the index, a single open stream, and one small result per
// calling thread. The vitaGL reader copies that result before its next call.
class CompactSeed {
    struct Entry { uint32_t kind, size; uint64_t key; uint32_t offset, crc; };
    static_assert(sizeof(Entry) == 24, "little-endian Vita seed index");
    FILE* file_ = nullptr;
    std::unique_ptr<Entry[]> rows_;
    uint32_t count_ = 0, data_ = 0;
    std::mutex mutex_;
    char root_[256]{};
    size_t rootSize_ = 0;
    static uint32_t CRC(const void* ptr, size_t n) {
        static constexpr uint32_t tab[] = {0,0x1db71064,0x3b6e20c8,0x26d930ac,0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,
            0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c};
        uint32_t c = ~0u;
        auto* p = static_cast<const uint8_t*>(ptr);
        while (n--) { c ^= *p++; c = (c >> 4) ^ tab[c & 15]; c = (c >> 4) ^ tab[c & 15]; }
        return ~c;
    }
public:
    CompactSeed() = default;
    ~CompactSeed() { if (file_) std::fclose(file_); }
    unsigned Entries() const { return count_; }
    bool Open(const char* name, const char* root, uint64_t abi) {
        if (file_ || !root || !name || !*root || std::strlen(root) >= sizeof root_) return false;
        std::unique_ptr<FILE, decltype(&std::fclose)> f(std::fopen(name, "rb"), &std::fclose);
        if (!f) return false;
        std::setvbuf(f.get(), nullptr, _IONBF, 0);
        struct Header { char magic[8]; uint32_t version, count; uint64_t abi; uint32_t bytes, crc; } h;
        static_assert(sizeof h == 32, "seed header");
        if (std::fread(&h, 1, sizeof h, f.get()) != sizeof h || std::memcmp(h.magic, "SMGSEED1", 8) ||
            h.version != 1 || h.abi != abi || !h.count || h.count > 8192 || h.bytes > (8u << 20)) return false;
        auto rows = std::unique_ptr<Entry[]>(new (std::nothrow) Entry[h.count]);
        const unsigned span = h.count * sizeof(Entry);
        if (!rows || std::fread(rows.get(), 1, span, f.get()) != span || CRC(rows.get(), span) != h.crc) return false;
        uint32_t at = 0;
        for (unsigned i = 0; i < h.count; ++i) {
            const auto& e = rows[i];
            if ((e.kind != 1 && e.kind != 2) || !e.size || e.size > 4096 || e.offset != at || e.size > h.bytes - at ||
                (i && (rows[i-1].kind > e.kind || (rows[i-1].kind == e.kind && rows[i-1].key >= e.key)))) return false;
            at += e.size;
        }
        if (at != h.bytes || std::fseek(f.get(), 0, SEEK_END) || std::ftell(f.get()) != long(sizeof h + span + at)) return false;
        rows_ = std::move(rows); count_ = h.count; data_ = sizeof h + span; file_ = f.release();
        std::strcpy(root_, root); rootSize_ = std::strlen(root_);
        return true;
    }
    int Index(const char* path) const {
        uint32_t kind; uint64_t key;
        if (!count_ || !ShaderPack::ParsePath(root_, rootSize_, path, kind, key)) return -1;
        uint32_t lo = 0, hi = count_;
        while (lo < hi) {
            const auto mid = lo + (hi - lo) / 2;
            const auto& r = rows_[mid];
            if (r.kind < kind || (r.kind == kind && r.key < key)) lo = mid + 1;
            else hi = mid;
        }
        return lo < count_ && rows_[lo].kind == kind && rows_[lo].key == key ? int(lo) : -1;
    }
    bool Read(int index, const void** data, size_t* size) {
        if (!data || !size) return false;
        *data = nullptr; *size = 0;
        if (index < 0 || unsigned(index) >= count_) return false;
        alignas(8) static thread_local uint8_t scratch[4096];
        const auto& e = rows_[index];
        {
            std::lock_guard<std::mutex> hold(mutex_);
            std::clearerr(file_);
            if (std::fseek(file_, data_ + e.offset, SEEK_SET) || std::fread(scratch, 1, e.size, file_) != e.size) return false;
        }
        if (CRC(scratch, e.size) != e.crc || !ShaderPack::ValidRecord(scratch, e.size, e.kind)) return false;
        *data = scratch; *size = e.size;
        return true;
    }
};
}
