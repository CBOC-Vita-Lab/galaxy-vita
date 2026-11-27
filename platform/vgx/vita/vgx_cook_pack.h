// cook24: the persistent user shader pack ("cooked asset" cache for the compiled shaders of this card's own play).
//
// What it is: the same VGXRAM01 image the RAM shader pack uses (vgx_shader_pack.h: sorted records, one read, answered from memory),
// holding the compiled shaders vitaGL found in loose files on the card (17-22 ms a link behind a failed open + a file open + a read, where a
// record from the pack costs ~4 ms) that neither the package's pack nor the card's stages.pack knew. It is written in the background a while after the
// title, in the card's idle time, and read in one fread at the next start.
//
// File (<folder>/user.pack.a and user.pack.b, written alternately; the newer valid one wins):
//   0   "VGXCOOK1"
//   8   u64 generation (1, 2, ...)
//   16  u32 record count            20  u32 offset of the image (32 + 4 * count, rounded up to 8)
//   24  u64 image size
//   32  u32 last-use generation per record (sorted-record order)   -> the LRU key
//   ..  the VGXRAM01 image, which carries the shader ABI and an XXH3 of its body (a stale ABI, a flipped bit or a cut file is refused)
// A torn write damages only the slot being written (the other keeps the previous generation); nothing is renamed, so FAT never sees
// a half-replaced file. The size is bounded (cap <= ShaderPack::kMaxBytes = 8 MiB, 8192 records); over it the records unused longest go.
// Everything here is plain stdio and std:: so that tests/vita_cook_pack.cpp runs it on the host.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "vgx_shader_pack.h"

namespace vgx::vita::cook {

constexpr size_t kPrefixBytes = 32;
constexpr size_t kMaxFileBytes = ShaderPack::kMaxBytes + kPrefixBytes + 4u * ShaderPack::kMaxEntries + 8;

struct Loaded {
    int slot = -1;                    // 0 = .a, 1 = .b
    uint64_t gen = 0;
    std::vector<uint32_t> ages;       // per record of the pack
    unsigned slotsSeen = 0, slotsRejected = 0;
};
struct NewRecord { uint32_t stage; uint64_t hash; std::vector<uint8_t> data; };
struct MergeStats { unsigned kept = 0, added = 0, evicted = 0, duplicate = 0; size_t bytes = 0; };

inline void SlotPath(char* out, size_t size, const char* folder, int slot) { std::snprintf(out, size, "%s/user.pack.%c", folder, slot ? 'b' : 'a'); }
inline uint32_t R32(const uint8_t* b) { return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24; }
inline uint64_t R64(const uint8_t* b) { return uint64_t(R32(b)) | uint64_t(R32(b + 4)) << 32; }
inline void W32(uint8_t* b, uint32_t v) { for (int k = 0; k < 4; ++k) b[k] = uint8_t(v >> (8 * k)); }
inline void W64(uint8_t* b, uint64_t v) { W32(b, uint32_t(v)); W32(b + 4, uint32_t(v >> 32)); }

// The generation of a slot from its first 32 bytes (0 = not a slot file).
inline bool PeekSlot(const char* path, uint64_t& gen) {
    gen = 0;
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    uint8_t h[kPrefixBytes];
    const bool ok = std::fread(h, 1, sizeof h, f) == sizeof h && std::memcmp(h, "VGXCOOK1", 8) == 0 && R64(h + 8) != 0;
    std::fclose(f);
    if (ok) gen = R64(h + 8);
    return ok;
}

// Reads and checks one slot file into `pack` (a validated image, root `root`). False: absent or refused.
inline bool LoadSlot(const char* path, const char* root, uint64_t abi, ShaderPack& pack, Loaded& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::setvbuf(f, nullptr, _IONBF, 0);
    bool ok = std::fseek(f, 0, SEEK_END) == 0;
    const long length = ok ? std::ftell(f) : -1;
    ok = ok && length >= static_cast<long>(kPrefixBytes + ShaderPack::kHeaderBytes) && static_cast<uint64_t>(length) <= kMaxFileBytes && std::fseek(f, 0, SEEK_SET) == 0;
    uint8_t* file = ok ? static_cast<uint8_t*>(std::malloc(static_cast<size_t>(length))) : nullptr;
    if (!file) { std::fclose(f); return false; }
    size_t got = 0;
    while (got < static_cast<size_t>(length)) {
        const size_t n = std::fread(file + got, 1, static_cast<size_t>(length) - got, f);
        if (!n) break;
        got += n;
    }
    ok = got == static_cast<size_t>(length) && !std::ferror(f);
    std::fclose(f);
    if (ok) {
        const uint64_t gen = R64(file + 8), imageSize = R64(file + 24);
        const uint32_t count = R32(file + 16), off = R32(file + 20);
        ok = std::memcmp(file, "VGXCOOK1", 8) == 0 && gen != 0 && count && count <= ShaderPack::kMaxEntries &&
             off == ((kPrefixBytes + 4u * count + 7u) & ~7u) && imageSize <= ShaderPack::kMaxBytes && uint64_t(off) + imageSize == uint64_t(length);
        if (ok) {
            std::vector<uint32_t> ages(count);
            for (uint32_t i = 0; i < count; ++i) ages[i] = R32(file + kPrefixBytes + 4u * i);
            std::memmove(file, file + off, static_cast<size_t>(imageSize));  // the image starts at the allocation again (free() still works)
            if (R32(file + 12) == count && pack.Adopt(file, static_cast<size_t>(imageSize), root, abi)) {
                out.gen = gen; out.ages = std::move(ages);
                return true;
            }
        }
    }
    std::free(file);
    return false;
}

// Both slots' first bytes say which is newer; that one is read, the other only when the newer is refused.
inline bool Load(const char* folder, const char* root, uint64_t abi, ShaderPack& pack, Loaded& out) {
    char path[2][288];
    uint64_t gen[2] = {0, 0};
    for (int s = 0; s < 2; ++s) {
        SlotPath(path[s], sizeof path[s], folder, s);
        if (PeekSlot(path[s], gen[s])) ++out.slotsSeen;
    }
    const int order[2] = {gen[1] > gen[0] ? 1 : 0, gen[1] > gen[0] ? 0 : 1};
    for (int k = 0; k < 2; ++k) {
        const int s = order[k];
        if (!gen[s]) continue;
        if (LoadSlot(path[s], root, abi, pack, out)) { out.slot = s; return true; }
        ++out.slotsRejected;
    }
    return false;
}

// The next file: the old pack's records (`used[i]` set = answered a lookup this run) plus `fresh`, cut to `capBytes` by last use (oldest first, then the bigger).
inline bool Merge(const ShaderPack& old, const Loaded& was, const std::vector<uint8_t>& used, const std::vector<NewRecord>& fresh, uint64_t abi,
                  size_t capBytes, std::vector<uint8_t>& file, MergeStats& st) {
    st = MergeStats();
    const uint64_t gen = was.gen + 1;
    struct Cand { ShaderPack::Record r; uint32_t age; };
    std::vector<Cand> cands;
    for (uint32_t i = 0; i < old.Entries(); ++i) {
        ShaderPack::Record r{};
        if (!old.At(i, r.stage, r.hash, r.data, r.length)) continue;
        const uint32_t was_age = i < was.ages.size() ? was.ages[i] : 0;
        cands.push_back({r, i < used.size() && used[i] ? static_cast<uint32_t>(gen) : was_age});
    }
    for (const NewRecord& n : fresh) {
        bool dup = false;
        for (const Cand& c : cands) if (c.r.stage == n.stage && c.r.hash == n.hash) { dup = true; break; }
        if (dup) { ++st.duplicate; continue; }
        if (!ShaderPack::ValidRecord(n.data.data(), static_cast<uint32_t>(n.data.size()), n.stage)) continue;
        cands.push_back({{n.stage, n.hash, n.data.data(), static_cast<uint32_t>(n.data.size())}, static_cast<uint32_t>(gen)});
        ++st.added;
    }
    if (capBytes > ShaderPack::kMaxBytes) capBytes = ShaderPack::kMaxBytes;
    // newest first; at the same age the smaller record stays (more shaders for the bytes)
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.age != b.age ? a.age > b.age : a.r.length < b.r.length; });
    std::vector<Cand> kept;
    size_t bytes = ShaderPack::kHeaderBytes;
    for (const Cand& c : cands) {
        const size_t need = ShaderPack::kEntryBytes + ((size_t(c.r.length) + 3u) & ~size_t(3));
        if (kept.size() >= ShaderPack::kMaxEntries || bytes + need > capBytes) { continue; }
        bytes += need;
        kept.push_back(c);
    }
    st.evicted = static_cast<unsigned>(cands.size() - kept.size());
    st.kept = static_cast<unsigned>(kept.size());
    if (kept.empty()) return false;
    std::sort(kept.begin(), kept.end(), [](const Cand& a, const Cand& b) { return a.r.stage != b.r.stage ? a.r.stage < b.r.stage : a.r.hash < b.r.hash; });
    std::vector<ShaderPack::Record> recs;
    for (const Cand& c : kept) recs.push_back(c.r);
    std::vector<uint8_t> image;
    if (!ShaderPack::Build(recs.data(), recs.size(), abi, image)) return false;
    const uint32_t count = static_cast<uint32_t>(kept.size()), off = static_cast<uint32_t>((kPrefixBytes + 4u * count + 7u) & ~7u);
    file.assign(static_cast<size_t>(off) + image.size(), 0);
    std::memcpy(file.data(), "VGXCOOK1", 8);
    W64(file.data() + 8, gen); W32(file.data() + 16, count); W32(file.data() + 20, off); W64(file.data() + 24, image.size());
    for (uint32_t i = 0; i < count; ++i) W32(file.data() + kPrefixBytes + 4u * i, kept[i].age);
    std::memcpy(file.data() + off, image.data(), image.size());
    st.bytes = file.size();
    return true;
}

// Writes the file into the slot that is not the one in use, in `chunk` byte pieces; `between` (optional) is called after each and may sleep
// or ask to stop (returns false: the slot is left unfinished, which a later start refuses by its checksum, the other slot is untouched).
inline bool Save(const char* folder, int currentSlot, const std::vector<uint8_t>& file, size_t chunk, bool (*between)(void*), void* user, int* slotWritten) {
    const int slot = currentSlot == 0 ? 1 : 0;
    char path[288];
    SlotPath(path, sizeof path, folder, slot);
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const size_t head = kPrefixBytes;
    // the prefix is written last (zeros first): a cut write leaves a slot with no magic, which PeekSlot ignores
    static const uint8_t zeros[kPrefixBytes] = {};
    bool ok = file.size() > head && std::fwrite(zeros, 1, head, f) == head;
    size_t at = head;
    while (ok && at < file.size()) {
        const size_t n = std::min(chunk ? chunk : file.size(), file.size() - at);
        ok = std::fwrite(file.data() + at, 1, n, f) == n;
        at += n;
        if (ok && between && at < file.size()) ok = between(user);
    }
    if (ok) ok = std::fseek(f, 0, SEEK_SET) == 0 && std::fwrite(file.data(), 1, head, f) == head;
    if (std::fclose(f) != 0) ok = false;
    if (ok && slotWritten) *slotWritten = slot;
    return ok;
}

}  // namespace vgx::vita::cook
