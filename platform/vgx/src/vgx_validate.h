#pragma once
#include "vgx_cmd.h"
#include <limits>
#include <unordered_map>

namespace vgx::validate {

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "validator requires IEEE-754 binary32");

struct Window {
    bool enabled = false, invalid = false;
    uint64_t first = 0, last = 0;
    bool Contains(uint64_t frame) const { return enabled && frame >= first && frame <= last; }
};

inline Window ParseWindow(const char* text) {
    Window w;
    if (!text || !*text || std::strcmp(text, "0") == 0) return w;
    auto number = [&](uint64_t& value) {
        if (*text < '0' || *text > '9') return false;
        do {
            const unsigned digit = *text++ - '0';
            if (value > (UINT64_MAX - digit) / 10) return false;
            value = value * 10 + digit;
        } while (*text >= '0' && *text <= '9');
        return true;
    };
    bool valid = number(w.first);
    if (valid && *text == ':') { ++text; valid = number(w.last); }
    else w.last = w.first;
    valid = valid && !*text && w.last >= w.first && w.last - w.first < 4;
    w.enabled = valid;
    w.invalid = !valid;
    return w;
}

constexpr uint64_t kHashStart = UINT64_C(14695981039346656037);
inline uint64_t Hash(const void* data, size_t size, uint64_t hash = kHashStart) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}
inline uint64_t HashWord(uint64_t value, uint64_t hash) {
    for (unsigned i = 0; i < 8; ++i) { const uint8_t byte = value & 255; hash = Hash(&byte, 1, hash); value >>= 8; }
    return hash;
}
inline bool FiniteWord(const void* data) {
    uint32_t word;
    std::memcpy(&word, data, 4);
    return (word & 0x7f800000u) != 0x7f800000u;
}

struct BufferView {
    const uint8_t* vertices = nullptr;
    size_t vertexBytes = 0;
    const uint16_t* indices = nullptr;
    size_t indexCount = 0;
};
struct UniformRange { uint32_t block, extra, count; };
struct Result {
    uint64_t vertexHash = kHashStart, indexHash = kHashStart, uniformHash = kHashStart;
    uint64_t vertexBytes = 0, indexBytes = 0, uniformBytes = 0;
    uint32_t errors = 0, references = 0, minIndex = 0, maxIndex = 0;
    uint32_t badIndexRange = 0, badVertexRefs = 0, outsideDeclaredRefs = 0, badUniformRanges = 0, badLayout = 0;
    uint32_t nonfinitePositionRefs = 0, nonfiniteAttributeRefs = 0, nonfiniteUniforms = 0;
    uint32_t nonfiniteByBlock[UB_COUNT] = {};
    uint32_t badPaletteRefs = 0, clampedPaletteRefs = 0;
    uint32_t firstPosition[4] = {};
};

inline Result Inspect(const DrawRec& draw, BufferView buffer, const float* uniforms, size_t uniformCount,
                      const UniformRange* ranges, size_t rangeCount, bool positionPalette, unsigned clampRow) {
    Result result;
    result.badLayout = (draw.layoutMask & ~0xFFFu) != 0;
    for (size_t i = 0; i < rangeCount; ++i) {
        const UniformRange& range = ranges[i];
        if (!range.count) continue;
        const uint64_t base = range.block < UB_COUNT ? draw.ublock[range.block] : kNoBlock;
        const uint64_t start = base + range.extra;
        if (base == kNoBlock || !uniforms || start > uniformCount || range.count > uniformCount - start) {
            ++result.badUniformRanges;
            continue;
        }
        const float* data = uniforms + static_cast<size_t>(start);
        result.uniformHash = HashWord(range.block, result.uniformHash);
        result.uniformHash = HashWord(range.extra, result.uniformHash);
        result.uniformHash = HashWord(range.count, result.uniformHash);
        result.uniformHash = Hash(data, range.count * sizeof(float), result.uniformHash);
        result.uniformBytes += range.count * sizeof(float);
        for (uint32_t j = 0; j < range.count; ++j) {
            if (!FiniteWord(data + j)) { ++result.nonfiniteUniforms; ++result.nonfiniteByBlock[range.block]; }
        }
    }
    if (draw.idxOffset > buffer.indexCount || draw.idxCount > buffer.indexCount - draw.idxOffset ||
        (draw.idxCount && !buffer.indices)) {
        result.badIndexRange = 1;
    } else {
        const VtxLayout layout = MakeLayout(draw.layoutMask);
        result.minIndex = draw.idxCount ? UINT16_MAX : 0;
        for (uint32_t i = 0; i < draw.idxCount; ++i) {
            const uint16_t index = buffer.indices[static_cast<size_t>(draw.idxOffset) + i];
            result.indexHash = Hash(&index, sizeof index, result.indexHash);
            result.indexBytes += sizeof index;
            ++result.references;
            if (index < result.minIndex) result.minIndex = index;
            if (index > result.maxIndex) result.maxIndex = index;
            if (index < draw.vtxFirst || index >= static_cast<uint64_t>(draw.vtxFirst) + draw.vtxCount)
                ++result.outsideDeclaredRefs;
            const uint64_t start = static_cast<uint64_t>(draw.vtxOffset) + static_cast<uint64_t>(index) * layout.stride;
            if (!buffer.vertices || start > buffer.vertexBytes || layout.stride > buffer.vertexBytes - start) {
                ++result.badVertexRefs;
                continue;
            }
            const uint8_t* vertex = buffer.vertices + static_cast<size_t>(start);
            if (!i) std::memcpy(result.firstPosition, vertex, sizeof result.firstPosition);
            result.vertexHash = Hash(vertex, layout.stride, result.vertexHash);
            result.vertexBytes += layout.stride;
            bool posFinite = true, attrFinite = true;
            for (unsigned k = 0; k < 4; ++k) posFinite &= FiniteWord(vertex + 4 * k);
            if (draw.layoutMask & VL_NRM)
                for (unsigned k = 0; k < 3; ++k) attrFinite &= FiniteWord(vertex + layout.offNrm + 4 * k);
            for (unsigned t = 0; t < 8; ++t)
                if (draw.layoutMask & (VL_TEX0 << t))
                    for (unsigned k = 0; k < 2; ++k) attrFinite &= FiniteWord(vertex + layout.offTex[t] + 4 * k);
            result.nonfinitePositionRefs += !posFinite;
            result.nonfiniteAttributeRefs += !attrFinite;
            if (positionPalette) {
                float row;
                std::memcpy(&row, vertex + 12, 4);
                if (!FiniteWord(vertex + 12) || row < 0 || row > 63 || row != static_cast<unsigned>(row)) ++result.badPaletteRefs;
                else if (row > clampRow) ++result.clampedPaletteRefs;
            }
        }
    }
    result.errors = result.badLayout + result.badIndexRange + result.badVertexRefs + result.outsideDeclaredRefs + result.badUniformRanges +
                    result.nonfinitePositionRefs + result.nonfiniteAttributeRefs + result.nonfiniteUniforms + result.badPaletteRefs;
    return result;
}

// Opt-in shadows keep static uploads inspectable after the source CmdBuf is released.
class Session {
    Window window;
    size_t limit, bytes = 0;
    std::unordered_map<uint32_t, BufUpload> shadows;
public:
    uint32_t shadowDrops = 0;
    explicit Session(Window w, size_t maxBytes = 16 * 1024 * 1024) : window(w), limit(maxBytes) {}
    bool Enabled() const { return window.enabled; }
    bool Selected(uint64_t frame) const { return window.Contains(frame); }
    size_t ShadowBytes() const { return bytes; }
    void Free(uint32_t id) {
        auto found = shadows.find(id);
        if (found == shadows.end()) return;
        bytes -= found->second.vtx.size() + found->second.idx.size() * sizeof(uint16_t);
        shadows.erase(found);
    }
    void Upload(const BufUpload& upload) {
        if (!Enabled()) return;
        Free(upload.bufId);
        const uint64_t size = static_cast<uint64_t>(upload.vtx.size()) + static_cast<uint64_t>(upload.idx.size()) * sizeof(uint16_t);
        if (size > limit - bytes) { ++shadowDrops; return; }
        shadows.emplace(upload.bufId, upload);
        bytes += static_cast<size_t>(size);
    }
    BufferView Find(uint32_t id) const {
        auto found = shadows.find(id);
        if (found == shadows.end()) return {};
        const BufUpload& upload = found->second;
        return {upload.vtx.data(), upload.vtx.size(), upload.idx.data(), upload.idx.size()};
    }
    void Finish(uint64_t frame) {
        if (Enabled() && frame >= window.last) { window.enabled = false; shadows.clear(); bytes = 0; }
    }
};

} // namespace vgx::validate
