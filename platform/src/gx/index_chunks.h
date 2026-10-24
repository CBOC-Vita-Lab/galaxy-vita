// U16 submission transport for immutable, already-expanded GX draw spans.
#pragma once

#include "draw_spans.h"

namespace gpu {

// GXM documents U16 attribute indices strictly below 64000, not 65536.
inline constexpr uint32_t kIndexChunkMaxVertices = 64000;

struct IndexChunkOrigin {
    size_t drawIndex = 0, sourceEventIndex = 0;
};

struct IndexChunk {
    size_t drawIndex = 0, sourceEventIndex = 0, commandWord = 0;
    size_t firstIndex = 0; // offset into the original DrawSpan::indices
    uint32_t primitive = 0, vtxFlags = 0, firstWord = 0, sourceVertexCount = 0, strideWords = 0;
    SpanTopology topology = SpanTopology::Points;
    std::shared_ptr<const DrawSpanState> state;
    // remap.size() is the chunk's local vertex count (never sourceVertexCount).
    // Local vertex -> original draw vertex ordinal. Upload source words at
    // firstWord + size_t(remap[local]) * strideWords, or gather CPU-XF output.
    std::vector<uint32_t> remap;
    std::vector<uint16_t> indices;
};

struct IndexChunkEvent {
    SpanEventKind kind;
    // Draw indexes chunks(); other kinds index sourceSpans()' typed vectors.
    size_t index = 0, sourceEventIndex = 0, commandWord = 0;
};

struct IndexChunkError {
    size_t drawIndex = 0, sourceEventIndex = 0, commandWord = 0, indexOffset = 0;
    std::string message;
};

class IndexChunks {
public:
    const Frame& frame() const { return *mFrame; }
    const std::shared_ptr<const Frame>& frameOwner() const { return mFrame; }
    // Null only for the single-draw entry point, which has no non-draw events.
    const std::shared_ptr<const DrawSpans>& sourceSpans() const { return mSource; }
    const std::vector<IndexChunk>& chunks() const { return mChunks; }
    const std::vector<IndexChunkEvent>& events() const { return mEvents; }

private:
    IndexChunks() = default;
    friend bool buildIndexChunks(std::shared_ptr<const DrawSpans>, std::shared_ptr<const IndexChunks>&, IndexChunkError&);
    friend bool buildDrawIndexChunks(std::shared_ptr<const Frame>, std::shared_ptr<const DrawSpan>,
                                    std::shared_ptr<const IndexChunks>&, IndexChunkError&, IndexChunkOrigin);
    std::shared_ptr<const Frame> mFrame;
    std::shared_ptr<const DrawSpans> mSource;
    std::vector<IndexChunk> mChunks;
    std::vector<IndexChunkEvent> mEvents;
};

// Preserve events exactly; replace each draw event with its ordered chunks.
// Empty index streams retain one empty chunk/event, never vanish from replay.
// All failure paths clear output and report source provenance; no partial result.
[[nodiscard]] bool buildIndexChunks(std::shared_ptr<const DrawSpans> source,
                                   std::shared_ptr<const IndexChunks>& output,
                                   IndexChunkError& error);

// Same transport for an independently owned immutable DrawSpan. Optional origin
// retains its frame-global draw/event ordinals. Frame words must remain frozen.
[[nodiscard]] bool buildDrawIndexChunks(std::shared_ptr<const Frame> frame,
                                       std::shared_ptr<const DrawSpan> draw,
                                       std::shared_ptr<const IndexChunks>& output,
                                       IndexChunkError& error, IndexChunkOrigin origin = {});

} // namespace gpu
