// packrefill25 (GXV_PACK_MISSLOG, default 0): the shader pack's miss log.
//
// vitaGL asks vgxvita_shaderCacheRead_v1 for every shader text it is about to compile (the name is the cache file's: <root>/<v|f>/<HH>/<HASH>.gxp,
// HASH = XXH3_64bits of the text as handed to glShaderSource). A lookup the packs do not answer is a "pack miss": a loose file on the card or in the
// package answers it, or the shader compiler runs. This header is the bookkeeping for the log a refill tour collects from:
//   * First(kind, hash)   true once per distinct key (a small locked set; 8192 keys per stage kind; a full table counts "overflow" and logs nothing more)
//   * FormatMiss          "pack-miss: f/3A/3A1B2C3D4E5F6071.gxp stage=099e9a42 play=1 t=123456 n=17": ONE line per first miss. The first token after the
//                         tag IS the pack record's name: tools/pack25/misskeys.py (and refill_pack.py's --expect) take exactly that.
//   * PackSummary         the per-minute line "pack: hits N misses M compiled-in-play K (...)" (deltas of the window; totals after "since start")
// Pure C++17, no Vita calls, so that tests/vita_pack_misslog.cpp runs it on the host. Nothing here is used unless the switch Init()s the set.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace vgx::vita {

class PackMissLog {
public:
    static constexpr uint32_t kSlots = 8192;  // per stage kind (power of two)
    PackMissLog() = default;
    PackMissLog(const PackMissLog&) = delete;
    PackMissLog& operator=(const PackMissLog&) = delete;
    ~PackMissLog() { std::free(table_); }

    // Startup, before any lookup thread runs. False when the memory is not there (the log stays off).
    bool Init() {
        if (table_) return true;
        // (a mutex and plain words, not 64-bit atomics: wait_wrap.cpp notes the Vita's ARM side has none; a miss costs a file read or a compile, the lock is never hot)
        table_ = static_cast<uint64_t*>(std::calloc(2u * kSlots, sizeof(uint64_t)));
        return table_ != nullptr;
    }
    bool Enabled() const { return table_ != nullptr; }

    // kind 1 = vertex, 2 = fragment (the pack's stage numbers). True the first time this key is met. Any thread.
    bool First(uint32_t kind, uint64_t hash) {
        if (!table_ || (kind != 1 && kind != 2)) return false;
        uint64_t* t = table_ + (kind - 1) * kSlots;
        const uint64_t want = hash ? hash : 1;  // (0 marks an empty slot; a real hash of 0 or 1 shares one entry: 2^-63)
        uint32_t i = static_cast<uint32_t>((want * 0x9E3779B97F4A7C15ull) >> 40) & (kSlots - 1);
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t probes = 0; probes < kSlots; ++probes, i = (i + 1) & (kSlots - 1)) {
            if (t[i] == want) return false;
            if (t[i] == 0) { t[i] = want; distinct_.fetch_add(1, std::memory_order_relaxed); return true; }
        }
        overflow_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    uint32_t Distinct() const { return distinct_.load(std::memory_order_relaxed); }
    uint32_t Overflow() const { return overflow_.load(std::memory_order_relaxed); }

    // The last three components of a cache file path ("<anything>/v/3A/3A1B2C3D4E5F6071.gxp" or the relative "v/3A/3A1B....gxp"):
    // kind, hash. The hash digits are upper-case hex without leading zeros, HH is the hash's top byte (as ShaderPack::ParsePath requires).
    static bool ParseName(const char* path, uint32_t& kind, uint64_t& hash) {
        if (!path) return false;
        const size_t n = std::strlen(path);
        if (n < 10 || n > 320) return false;
        if (std::strcmp(path + n - 4, ".gxp") != 0) return false;
        const char* end = path + n - 4;                 // one past the last hex digit
        const char* c = end;
        while (c > path && c[-1] != '/') --c;           // c: first hex digit of HASH
        const size_t digits = static_cast<size_t>(end - c);
        if (digits < 1 || digits > 16 || c - path < 5) return false;
        if (c[-1] != '/' || c[-4] != '/' || (c - path > 5 && c[-6] != '/')) return false;
        const char k = c[-5];
        if (k != 'v' && k != 'f') return false;
        const int a = Hex(c[-3]), b = Hex(c[-2]);
        if (a < 0 || b < 0 || (digits > 1 && c[0] == '0')) return false;
        uint64_t h = 0;
        for (size_t i = 0; i < digits; ++i) {
            const int d = Hex(c[i]);
            if (d < 0) return false;
            h = (h << 4) | static_cast<unsigned>(d);
        }
        if (static_cast<unsigned>(h >> 56) != static_cast<unsigned>(a * 16 + b)) return false;
        kind = k == 'v' ? 1u : 2u;
        hash = h;
        return true;
    }

    // "pack-miss: <v|f>/<HH>/<HASH>.gxp stage=<8 hex> play=<0|1> t=<ms> n=<ordinal of this miss, from 1>". Returns the length (0: buffer too small).
    static int FormatMiss(char* out, size_t size, uint32_t kind, uint64_t hash, uint32_t stage, int play, uint64_t tMs, uint32_t ordinal) {
        const int n = std::snprintf(out, size, "pack-miss: %c/%02X/%llX.gxp stage=%08x play=%d t=%llu n=%u", kind == 1 ? 'v' : 'f',
                                    static_cast<unsigned>(hash >> 56), static_cast<unsigned long long>(hash), static_cast<unsigned>(stage), play ? 1 : 0,
                                    static_cast<unsigned long long>(tMs), static_cast<unsigned>(ordinal));
        return n > 0 && static_cast<size_t>(n) < size ? n : 0;
    }
    // The grammar the offline tool reads: a line holding "pack-miss: <name>" anywhere (the log's own prefix may come before it).
    static bool ParseMissLine(const char* line, uint32_t& kind, uint64_t& hash) {
        const char* p = line ? std::strstr(line, "pack-miss: ") : nullptr;
        if (!p) return false;
        p += 11;
        char name[40];
        size_t n = 0;
        while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\r' && p[n] != '\n' && n < sizeof name - 1) { name[n] = p[n]; ++n; }
        name[n] = 0;
        return ParseName(name, kind, hash);
    }

private:
    static int Hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
    uint64_t* table_ = nullptr;
    std::mutex mutex_;
    std::atomic<uint32_t> distinct_{0}, overflow_{0};
};

// The per-window line. The caller owns the totals (atomics of the host); Tick() turns them into the window's deltas once a period has passed.
class PackSummary {
public:
    struct Totals { uint64_t hits = 0, misses = 0, compiles = 0, compilesInPlay = 0, userHits = 0; uint32_t distinct = 0, overflow = 0; };
    explicit PackSummary(uint64_t periodMs = 60000) : period_(periodMs ? periodMs : 60000) {}
    // True when a line is due at `nowMs` (the first call only starts the window). Single caller (the render thread).
    bool Tick(uint64_t nowMs, const Totals& now, Totals& delta, uint64_t& windowMs) {
        if (!started_) { started_ = true; at_ = nowMs; last_ = now; return false; }
        if (nowMs < at_ || nowMs - at_ < period_) return false;
        delta.hits = now.hits - last_.hits;
        delta.misses = now.misses - last_.misses;
        delta.compiles = now.compiles - last_.compiles;
        delta.compilesInPlay = now.compilesInPlay - last_.compilesInPlay;
        delta.userHits = now.userHits - last_.userHits;
        delta.distinct = now.distinct;
        delta.overflow = now.overflow;
        windowMs = nowMs - at_;
        at_ = nowMs;
        last_ = now;
        return true;
    }
    // "pack: hits N misses M compiled-in-play K (window S s: compiled C in all, user pack U; since start: hits .. misses .. (D distinct keys, O over the table) compiled-in-play ..)"
    static int FormatSummary(char* out, size_t size, const Totals& d, uint64_t windowMs, const Totals& total) {
        const int n = std::snprintf(out, size,
            "pack: hits %llu misses %llu compiled-in-play %llu (window %llu s: compiled %llu in all, user pack hits %llu; since start: hits %llu misses %llu "
            "(%u distinct keys, %u past the table) compiled %llu, compiled-in-play %llu)",
            static_cast<unsigned long long>(d.hits), static_cast<unsigned long long>(d.misses), static_cast<unsigned long long>(d.compilesInPlay),
            static_cast<unsigned long long>((windowMs + 500) / 1000), static_cast<unsigned long long>(d.compiles), static_cast<unsigned long long>(d.userHits),
            static_cast<unsigned long long>(total.hits), static_cast<unsigned long long>(total.misses), static_cast<unsigned>(total.distinct),
            static_cast<unsigned>(total.overflow), static_cast<unsigned long long>(total.compiles), static_cast<unsigned long long>(total.compilesInPlay));
        return n > 0 && static_cast<size_t>(n) < size ? n : 0;
    }
private:
    uint64_t period_, at_ = 0;
    bool started_ = false;
    Totals last_;
};

}  // namespace vgx::vita
