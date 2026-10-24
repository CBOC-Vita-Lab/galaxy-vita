#include "index_chunks.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace gpu {

namespace {

bool appendDraw(const Frame& frame, const DrawSpan& draw, IndexChunkOrigin origin,
                std::vector<IndexChunk>& chunks, std::vector<IndexChunkEvent>& events,
                IndexChunkError& error) {
    auto fail = [&](size_t offset, const char* message) {
        error = {origin.drawIndex, origin.sourceEventIndex, draw.commandWord, offset, message};
        return false;
    };
    if (!draw.state) return fail(0, "draw has no immutable state");
    if ((draw.vtxFlags & ~0x3FFFu) || ((draw.vtxFlags & VF_NBT) && !(draw.vtxFlags & VF_NRM)))
        return fail(0, "invalid decoded vertex flags");
    if (draw.strideWords != vtxWords(draw.vtxFlags)) return fail(0, "draw stride disagrees with decoded vertex flags");
    if (draw.state->uid.vtxFlags != draw.vtxFlags) return fail(0, "shader state disagrees with decoded vertex flags");
    uint64_t words = uint64_t(draw.vertexCount) * draw.strideWords;
    if (draw.firstWord > frame.verts.size() || words > frame.verts.size() - draw.firstWord)
        return fail(0, "draw vertex range outside owned frame words");
    if (draw.primitive > 255) return fail(0, "primitive is not a GX opcode byte");
    SpanTopology expected;
    switch (draw.primitive & 0xF8) {
    case 0x80: case 0x88: case 0x90: case 0x98: case 0xA0: expected = SpanTopology::Triangles; break;
    case 0xA8: case 0xB0: expected = SpanTopology::Lines; break;
    case 0xB8: expected = SpanTopology::Points; break;
    default: return fail(0, "unknown GX primitive");
    }
    if (draw.topology != expected) return fail(0, "topology disagrees with expanded GX primitive");
    size_t arity = expected == SpanTopology::Triangles ? 3 : expected == SpanTopology::Lines ? 2 : 1;
    size_t tail = draw.indices.size() % arity;
    if (tail) return fail(draw.indices.size() - tail, "index stream ends inside a primitive");

    auto fresh = [&](size_t firstIndex) {
        IndexChunk chunk;
        chunk.drawIndex = origin.drawIndex; chunk.sourceEventIndex = origin.sourceEventIndex;
        chunk.commandWord = draw.commandWord; chunk.firstIndex = firstIndex;
        chunk.primitive = draw.primitive; chunk.vtxFlags = draw.vtxFlags;
        chunk.firstWord = draw.firstWord; chunk.sourceVertexCount = draw.vertexCount; chunk.strideWords = draw.strideWords;
        chunk.topology = draw.topology; chunk.state = draw.state;
        return chunk;
    };
    IndexChunk chunk = fresh(0);
    // Bounded by the transport's vertex cap, not the potentially huge/sparse
    // source vertexCount. First-use insertion determines remap order; never
    // iterate the hash table to construct vertex or index output.
    std::unordered_map<uint32_t, uint16_t> local;
    local.reserve(std::min<size_t>(kIndexChunkMaxVertices, std::min<size_t>(draw.vertexCount, draw.indices.size())));
    auto emit = [&] {
        events.push_back({SpanEventKind::Draw, chunks.size(), origin.sourceEventIndex, draw.commandWord});
        chunks.push_back(std::move(chunk));
    };
    for (size_t at = 0; at < draw.indices.size(); at += arity) {
        size_t needed = 0;
        for (size_t i = 0; i < arity; ++i) {
            uint32_t ordinal = draw.indices[at + i];
            if (ordinal >= draw.vertexCount) return fail(at + i, "index outside original draw vertex range");
            if (local.find(ordinal) != local.end()) continue;
            bool duplicate = false;
            for (size_t j = 0; j < i; ++j) duplicate |= draw.indices[at + j] == ordinal;
            if (!duplicate) ++needed;
        }
        // Preflight the whole primitive, including repeated new ordinals.
        // After a flush, reinsert its vertices in first-use order; old boundary
        // vertices may be duplicated in the upload, never in the geometry.
        if (chunk.remap.size() + needed > kIndexChunkMaxVertices) {
            emit();
            chunk = fresh(at);
            local.clear();
        }
        for (size_t i = 0; i < arity; ++i) {
            uint32_t ordinal = draw.indices[at + i];
            auto found = local.find(ordinal);
            uint16_t index;
            if (found == local.end()) {
                if (chunk.remap.size() >= kIndexChunkMaxVertices)
                    return fail(at + i, "primitive exceeds U16 vertex transport cap");
                index = static_cast<uint16_t>(chunk.remap.size());
                local.emplace(ordinal, index);
                chunk.remap.push_back(ordinal);
            } else index = found->second;
            chunk.indices.push_back(index);
        }
    }
    emit(); // Includes one explicit empty draw event when the stream is empty.
    return true;
}

} // namespace

bool buildIndexChunks(std::shared_ptr<const DrawSpans> source, std::shared_ptr<const IndexChunks>& output,
                      IndexChunkError& error) {
    output.reset();
    error = {};
    if (!source || !source->frameOwner()) {
        error.message = "null draw-span source or frame owner";
        return false;
    }
    auto result = std::shared_ptr<IndexChunks>(new IndexChunks);
    result->mSource = std::move(source);
    result->mFrame = result->mSource->frameOwner();
    const DrawSpans& spans = *result->mSource;
    size_t previousWord = 0;
    bool havePrevious = false;
    for (size_t ei = 0; ei < spans.events().size(); ++ei) {
        const SpanEvent& event = spans.events()[ei];
        auto fail = [&](const char* message) {
            error = {event.kind == SpanEventKind::Draw ? event.index : 0, ei, event.commandWord, 0, message};
            return false;
        };
        if (havePrevious && event.commandWord <= previousWord) return fail("source events are not in command order");
        previousWord = event.commandWord; havePrevious = true;
        switch (event.kind) {
        case SpanEventKind::Draw: {
            if (event.index >= spans.draws().size()) return fail("draw event index outside source spans");
            const DrawSpan& draw = spans.draws()[event.index];
            if (draw.commandWord != event.commandWord) return fail("draw/event command provenance disagrees");
            if (!appendDraw(result->frame(), draw, {event.index, ei}, result->mChunks, result->mEvents, error)) return false;
            break;
        }
        case SpanEventKind::Copy:
            if (event.index >= spans.copies().size()) return fail("copy event index outside source spans");
            result->mEvents.push_back({event.kind, event.index, ei, event.commandWord});
            break;
        case SpanEventKind::Marker:
            if (event.index >= spans.markers().size()) return fail("marker event index outside source spans");
            result->mEvents.push_back({event.kind, event.index, ei, event.commandWord});
            break;
        case SpanEventKind::Camera:
            if (event.index >= spans.cameras().size()) return fail("camera event index outside source spans");
            result->mEvents.push_back({event.kind, event.index, ei, event.commandWord});
            break;
        default: return fail("unknown source event kind");
        }
    }
    output = std::move(result);
    return true;
}

bool buildDrawIndexChunks(std::shared_ptr<const Frame> frame, std::shared_ptr<const DrawSpan> draw,
                          std::shared_ptr<const IndexChunks>& output, IndexChunkError& error, IndexChunkOrigin origin) {
    output.reset();
    error = {};
    if (!frame || !draw) {
        error = {origin.drawIndex, origin.sourceEventIndex, draw ? draw->commandWord : 0, 0, "null frame owner or draw span"};
        return false;
    }
    auto result = std::shared_ptr<IndexChunks>(new IndexChunks);
    result->mFrame = std::move(frame);
    if (!appendDraw(result->frame(), *draw, origin, result->mChunks, result->mEvents, error)) return false;
    output = std::move(result);
    return true;
}

} // namespace gpu
