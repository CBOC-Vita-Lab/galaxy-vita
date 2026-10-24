#include "index_chunks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <initializer_list>
#include <numeric>
#include <type_traits>

// Only the TexImage lifetime hook is supplied here: no GL objects exist.
namespace gpu { TexImage::~TexImage() = default; }

namespace {
using namespace gpu;
size_t checks = 0;
#define CHECK(test) do { ++checks; if (!(test)) { \
    fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #test); exit(1); } } while (0)

uint32_t bits(float f) { uint32_t word; memcpy(&word, &f, 4); return word; }
void cmd(Frame& f, std::initializer_list<uint32_t> words) {
    f.cmds.insert(f.cmds.end(), words.begin(), words.end());
}
void draw(Frame& f, uint32_t primitive, uint32_t count, uint32_t flags = 0) {
    uint32_t first = static_cast<uint32_t>(f.verts.size());
    for (uint32_t i = 0; i < count; ++i) {
        f.verts.push_back(bits(float(i)));
        f.verts.push_back(bits(float(i) / 8));
        f.verts.push_back(bits(-float(i)));
        for (uint32_t k = 3; k < vtxWords(flags); ++k) f.verts.push_back(0);
    }
    cmd(f, {CMD_DRAW, primitive, flags, first, count});
}
std::shared_ptr<const DrawSpans> spans(std::shared_ptr<Frame> f) {
    DrawSpanError error;
    std::shared_ptr<const DrawSpans> result;
    bool ok = buildDrawSpans(f, result, error);
    if (!ok) fprintf(stderr, "draw-span failure: word=%zu opcode=%u: %s\n",
                     error.commandWord, error.opcode, error.message.c_str());
    CHECK(ok && result);
    return result;
}
std::shared_ptr<const IndexChunks> chunks(std::shared_ptr<const DrawSpans> source) {
    IndexChunkError error;
    std::shared_ptr<const IndexChunks> result;
    bool ok = buildIndexChunks(std::move(source), result, error);
    if (!ok) fprintf(stderr, "chunk failure: draw=%zu event=%zu word=%zu index=%zu: %s\n",
                     error.drawIndex, error.sourceEventIndex, error.commandWord, error.indexOffset, error.message.c_str());
    CHECK(ok && result && error.message.empty());
    return result;
}
std::shared_ptr<const IndexChunks> single(std::shared_ptr<const Frame> frame,
                                         std::shared_ptr<const DrawSpan> source, IndexChunkOrigin origin = {}) {
    IndexChunkError error;
    std::shared_ptr<const IndexChunks> result;
    bool ok = buildDrawIndexChunks(std::move(frame), std::move(source), result, error, origin);
    if (!ok) fprintf(stderr, "single chunk failure: draw=%zu event=%zu word=%zu index=%zu: %s\n",
                     error.drawIndex, error.sourceEventIndex, error.commandWord, error.indexOffset, error.message.c_str());
    CHECK(ok && result && error.message.empty());
    return result;
}

void roundTrip(const IndexChunks& out, const DrawSpan& source, IndexChunkOrigin origin = {}) {
    size_t cursor = 0;
    unsigned arity = source.topology == SpanTopology::Triangles ? 3 : source.topology == SpanTopology::Lines ? 2 : 1;
    CHECK(!out.chunks().empty());
    CHECK(out.events().size() == out.chunks().size());
    for (size_t ci = 0; ci < out.chunks().size(); ++ci) {
        const auto& chunk = out.chunks()[ci];
        CHECK(chunk.drawIndex == origin.drawIndex && chunk.sourceEventIndex == origin.sourceEventIndex);
        CHECK(chunk.commandWord == source.commandWord && chunk.firstIndex == cursor);
        CHECK(chunk.firstWord == source.firstWord && chunk.strideWords == source.strideWords);
        CHECK(chunk.sourceVertexCount == source.vertexCount && chunk.vtxFlags == source.vtxFlags);
        CHECK(chunk.primitive == source.primitive && chunk.topology == source.topology && chunk.state == source.state);
        CHECK(chunk.remap.size() <= kIndexChunkMaxVertices && chunk.indices.size() % arity == 0);
        CHECK(out.events()[ci].kind == SpanEventKind::Draw && out.events()[ci].index == ci);
        CHECK(out.events()[ci].sourceEventIndex == origin.sourceEventIndex && out.events()[ci].commandWord == source.commandWord);
        std::vector<uint32_t> unique = chunk.remap;
        std::sort(unique.begin(), unique.end());
        CHECK(std::adjacent_find(unique.begin(), unique.end()) == unique.end());
        for (uint32_t ordinal : chunk.remap) {
            CHECK(ordinal < source.vertexCount);
            size_t at = chunk.firstWord + size_t(ordinal) * chunk.strideWords;
            CHECK(at + chunk.strideWords <= out.frame().verts.size());
            CHECK(out.frame().verts[at] == bits(float(ordinal)));
            // Gather the exact recorded native words, including the nonzero
            // frame offset and all optional attribute words; no reinterpretation.
            for (uint32_t k = 0; k < chunk.strideWords; ++k)
                CHECK(out.frame().verts[at + k] == out.frame().verts[source.firstWord + size_t(ordinal) * source.strideWords + k]);
        }
        for (uint16_t local : chunk.indices) {
            CHECK(local < 64000); // VitaSDK's documented GXM U16 attribute-index limit.
            CHECK(local < chunk.remap.size());
            CHECK(cursor < source.indices.size() && chunk.remap[local] == source.indices[cursor]);
            ++cursor;
        }
    }
    CHECK(cursor == source.indices.size());
}

void pointBoundaries() {
    for (uint32_t count : {0u, 1u, 63999u, 64000u, 64001u, 65536u, 131071u}) {
        auto f = std::make_shared<Frame>();
        draw(*f, 0xBD, count); // preserve the original VAT bits, not just topology
        auto source = spans(f);
        auto out = chunks(source);
        roundTrip(*out, source->draws()[0]);
        size_t expected = count ? (size_t(count) + 63999) / 64000 : 1;
        CHECK(out->chunks().size() == expected);
        if (count >= 64000) {
            CHECK(out->chunks()[0].remap.size() == 64000);
            CHECK(out->chunks()[0].indices.back() == 63999);
        }
        if (count > 64000) {
            CHECK(out->chunks()[1].remap[0] == 64000);
            CHECK(out->chunks()[1].indices[0] == 0);
        }
    }
}

void wholeLinesAndTriangles() {
    for (uint32_t primitive : {0x90u, 0xA8u}) {
        auto f = std::make_shared<Frame>();
        draw(*f, primitive, 65538);
        auto source = spans(f);
        auto out = chunks(source);
        roundTrip(*out, source->draws()[0]);
        CHECK(out->chunks().size() == 2);
        size_t first = primitive == 0x90 ? 63999 : 64000;
        CHECK(out->chunks()[0].indices.size() == first && out->chunks()[0].remap.size() == first);
        CHECK(out->chunks()[1].firstIndex == first);
        CHECK(out->chunks()[1].remap[0] == first);
        CHECK(out->chunks()[1].indices[0] == 0);
    }
    // Duplicate new vertices inside one primitive count only once. Existing
    // primitives still fit in a full map, and degenerates are never removed.
    auto f = std::make_shared<Frame>(); draw(*f, 0xB8, 65537);
    auto source = spans(f);
    auto triangle = std::make_shared<DrawSpan>(source->draws()[0]);
    triangle->primitive = 0x90; triangle->topology = SpanTopology::Triangles;
    triangle->indices.resize(63996);
    std::iota(triangle->indices.begin(), triangle->indices.end(), 0u);
    triangle->indices.insert(triangle->indices.end(), {63996, 63997, 0, 63998, 63999, 1, 0, 1, 2, 64000, 64000, 64000, 1, 64001, 1});
    auto out = single(f, triangle, {12, 34});
    roundTrip(*out, *triangle, {12, 34});
    CHECK(out->chunks().size() == 2);
    CHECK(out->chunks()[0].remap.size() == 64000);
    CHECK(out->chunks()[0].indices.size() == 64005);
    CHECK(out->chunks()[1].remap == (std::vector<uint32_t>{64000, 1, 64001}));
    CHECK(out->chunks()[1].indices == (std::vector<uint16_t>{0, 0, 0, 1, 2, 1}));
    CHECK(!out->sourceSpans());
    auto line = std::make_shared<DrawSpan>(source->draws()[0]);
    line->primitive = 0xA8; line->topology = SpanTopology::Lines;
    line->indices.resize(63998);
    std::iota(line->indices.begin(), line->indices.end(), 0u);
    line->indices.insert(line->indices.end(), {63998, 63999, 0, 1, 64000, 64001});
    out = single(f, line);
    roundTrip(*out, *line);
    CHECK(out->chunks().size() == 2 && out->chunks()[0].remap.size() == 64000);
    CHECK(out->chunks()[0].indices.size() == 64002 && out->chunks()[1].firstIndex == 64002);
    CHECK(out->chunks()[1].remap == (std::vector<uint32_t>{64000, 64001}));
}

void sparseRepeatedAndUploadWords() {
    auto f = std::make_shared<Frame>();
    // The draw need not start at frame word zero; sparse high ordinals must
    // be remapped rather than narrowed or used as GPU buffer offsets.
    f->verts.assign(11, 0xDEADBEEF);
    uint32_t flags = VF_NRM | VF_NBT | VF_CLR0 | VF_CLR1 | VF_TEX0 | (VF_TEX0 << 7) | VF_TEXMTX;
    draw(*f, 0xB8, 200003, flags);
    auto source = spans(f);
    auto sparse = std::make_shared<DrawSpan>(source->draws()[0]);
    sparse->indices = {200002, 17, 200002, 65535, 17, 0, 131070, 200002};
    auto out = single(f, sparse, {123, 456});
    roundTrip(*out, *sparse, {123, 456});
    CHECK(out->chunks().size() == 1);
    CHECK(out->chunks()[0].remap == (std::vector<uint32_t>{200002, 17, 65535, 0, 131070}));
    CHECK(out->chunks()[0].indices == (std::vector<uint16_t>{0, 1, 0, 2, 1, 3, 4, 0}));
    CHECK(out->chunks()[0].firstWord == 11);
    CHECK(out->frame().verts[11 + size_t(200002) * vtxWords(flags)] == bits(200002));
    auto repeated = std::make_shared<DrawSpan>(*sparse);
    repeated->primitive = 0x90; repeated->topology = SpanTopology::Triangles;
    repeated->indices.assign(90000, 200002);
    out = single(f, repeated);
    roundTrip(*out, *repeated);
    CHECK(out->chunks().size() == 1 && out->chunks()[0].indices.size() == 90000);
    CHECK(out->chunks()[0].remap == (std::vector<uint32_t>{200002}));
    CHECK(std::all_of(out->chunks()[0].indices.begin(), out->chunks()[0].indices.end(), [](uint16_t i) { return i == 0; }));
}

void expandedTopologyAndDeterminism() {
    for (uint32_t primitive : {0x80u, 0x88u, 0x98u, 0xA0u, 0xB0u}) {
        auto f = std::make_shared<Frame>(); draw(*f, primitive, 65540);
        auto source = spans(f);
        auto a = chunks(source), b = chunks(source);
        roundTrip(*a, source->draws()[0]);
        CHECK(a->chunks().size() == b->chunks().size());
        for (size_t i = 0; i < a->chunks().size(); ++i) {
            CHECK(a->chunks()[i].remap == b->chunks()[i].remap);
            CHECK(a->chunks()[i].indices == b->chunks()[i].indices);
        }
        CHECK(a->chunks().size() == 2);
    }
    auto f = std::make_shared<Frame>(); draw(*f, 0xB8, 1001);
    auto source = spans(f);
    uint32_t random = 0x12345678;
    for (unsigned arity = 1; arity <= 3; ++arity) {
        auto s = std::make_shared<DrawSpan>(source->draws()[0]);
        s->primitive = arity == 1 ? 0xB8 : arity == 2 ? 0xA8 : 0x90;
        s->topology = arity == 1 ? SpanTopology::Points : arity == 2 ? SpanTopology::Lines : SpanTopology::Triangles;
        s->indices.clear();
        for (unsigned i = 0; i < 4000 * arity; ++i) {
            random = random * 1664525u + 1013904223u;
            s->indices.push_back(random % 1001);
        }
        auto out = single(f, s);
        roundTrip(*out, *s);
        CHECK(out->chunks().size() == 1);
    }
}

void eventOrderAndLifetime() {
    auto f = std::make_shared<Frame>();
    f->number = 1234;
    auto image = std::make_shared<TexImage>();
    image->width = image->height = 1; image->rgba.push_back(0x11223344);
    f->textures.push_back(image);
    cmd(*f, {CMD_CAMERA}); f->cmds.insert(f->cmds.end(), kCameraWords, 0);
    cmd(*f, {CMD_TEX, 0, 0});
    cmd(*f, {CMD_MARKER, MARK_PLAYER_BEGIN});
    draw(*f, 0xB8, 65536);
    cmd(*f, {CMD_EFB_COPY, 5, 1, 2, 3, 4, 5, 6, 7, 8, 1 << 11});
    cmd(*f, {CMD_MARKER, MARK_SCENE_DEPTH});
    cmd(*f, {CMD_MARKER, MARK_HUD_BEGIN});
    draw(*f, 0x98, 2); // legal empty index stream, preserve its event
    cmd(*f, {CMD_MARKER, MARK_HUD_END});
    draw(*f, 0x90, 3);
    draw(*f, 0x90, 3); // same snapshot, still separate draw/events
    cmd(*f, {CMD_MARKER, MARK_PLAYER_END});
    auto source = spans(f);
    auto out = chunks(source);
    CHECK(out->sourceSpans() == source && out->frameOwner() == source->frameOwner());
    CHECK(out->frame().number == 1234);
    size_t next = 0;
    for (size_t ei = 0; ei < source->events().size(); ++ei) {
        const auto& original = source->events()[ei];
        CHECK(next < out->events().size());
        if (original.kind != SpanEventKind::Draw) {
            const auto& event = out->events()[next++];
            CHECK(event.kind == original.kind && event.index == original.index);
            CHECK(event.sourceEventIndex == ei && event.commandWord == original.commandWord);
        } else {
            size_t lifted = 0, count = 0;
            const auto& d = source->draws()[original.index];
            while (next < out->events().size() && out->events()[next].sourceEventIndex == ei) {
                const auto& event = out->events()[next++];
                CHECK(event.kind == SpanEventKind::Draw && event.commandWord == original.commandWord);
                const auto& c = out->chunks()[event.index];
                CHECK(c.drawIndex == original.index && c.sourceEventIndex == ei && c.commandWord == d.commandWord);
                CHECK(c.firstIndex == lifted && c.state == d.state);
                for (uint16_t local : c.indices) CHECK(c.remap[local] == d.indices[lifted++]);
                ++count;
            }
            CHECK(count > 0 && lifted == d.indices.size());
        }
    }
    CHECK(next == out->events().size());
    CHECK(out->events().size() == source->events().size() + 1);
    CHECK(out->chunks().size() == 5);
    CHECK(out->chunks()[2].indices.empty() && out->chunks()[2].remap.empty());
    CHECK(out->chunks()[3].drawIndex != out->chunks()[4].drawIndex);
    CHECK(out->chunks()[3].state == out->chunks()[4].state);
    std::weak_ptr<Frame> frameLife = f;
    std::weak_ptr<TexImage> textureLife = image;
    std::weak_ptr<const DrawSpans> spanLife = source;
    f.reset(); image.reset(); source.reset();
    CHECK(!frameLife.expired() && !textureLife.expired() && !spanLife.expired());
    CHECK(out->frame().textures[0]->rgba[0] == 0x11223344);
    out.reset();
    CHECK(frameLife.expired() && textureLife.expired() && spanLife.expired());
    auto empty = spans(std::make_shared<Frame>());
    auto noEvents = chunks(empty);
    CHECK(noEvents->events().empty() && noEvents->chunks().empty());
}

void malformedTransactions() {
    auto f = std::make_shared<Frame>(); draw(*f, 0xB8, 65540);
    auto source = spans(f);
    auto prior = chunks(source);
    IndexChunkOrigin origin{77, 88};
    auto reject = [&](std::shared_ptr<const Frame> frame, std::shared_ptr<const DrawSpan> s, size_t at) {
        auto out = prior;
        IndexChunkError error;
        CHECK(!buildDrawIndexChunks(std::move(frame), s, out, error, origin));
        CHECK(!out && !error.message.empty());
        CHECK(error.drawIndex == 77 && error.sourceEventIndex == 88 && error.indexOffset == at);
        if (s) CHECK(error.commandWord == s->commandWord);
    };
    auto modified = [&] { return std::make_shared<DrawSpan>(source->draws()[0]); };
    auto s = modified(); s->commandWord = 999; s->indices.back() = s->vertexCount;
    reject(f, s, s->indices.size() - 1); // reject after the first complete chunk
    s = modified(); s->indices[0] = 0xFFFFFFFF; reject(f, s, 0);
    s = modified(); s->firstWord = 0xFFFFFFFF; reject(f, s, 0);
    s = modified(); s->vertexCount = 0xFFFFFFFF; reject(f, s, 0);
    s = modified(); s->vertexCount += 1; reject(f, s, 0);
    s = modified(); s->strideWords += 1; reject(f, s, 0);
    s = modified(); s->vtxFlags = 1 << 14; reject(f, s, 0);
    s = modified(); s->vtxFlags = VF_NBT; s->strideWords = vtxWords(VF_NBT); reject(f, s, 0);
    s = modified(); s->topology = static_cast<SpanTopology>(123); reject(f, s, 0);
    s = modified(); s->topology = SpanTopology::Lines; reject(f, s, 0);
    s = modified(); s->primitive = 0xC0; reject(f, s, 0);
    s = modified(); s->primitive = 0x1B8; reject(f, s, 0);
    s = modified(); s->state.reset(); reject(f, s, 0);
    s = modified(); s->vtxFlags = VF_CLR0; s->strideWords = vtxWords(VF_CLR0); reject(f, s, 0);
    s = modified(); s->topology = SpanTopology::Triangles; s->primitive = 0x90;
    s->indices = {0, 1}; reject(f, s, 0);
    s = modified(); s->topology = SpanTopology::Lines; s->primitive = 0xA8;
    s->indices = {0}; reject(f, s, 0);
    s = modified(); s->topology = SpanTopology::Triangles; s->primitive = 0x90;
    s->indices = {0, 1, 2, 3, 4}; reject(f, s, 3);
    s = modified(); s->topology = SpanTopology::Lines; s->primitive = 0xA8;
    s->indices = {0, 1, 2}; reject(f, s, 2);
    s = modified(); s->vertexCount = 0; s->indices = {0}; reject(f, s, 0);
    s = modified(); reject(nullptr, s, 0); reject(f, nullptr, 0);
    auto out = prior;
    IndexChunkError error;
    CHECK(!buildIndexChunks(nullptr, out, error) && !out && !error.message.empty());
    CHECK(prior->chunks().size() == 2 && prior->chunks()[1].remap.back() == 65539);
    auto only = std::make_shared<Frame>(); draw(*only, 0xB8, 3);
    auto ownSpans = spans(only);
    auto ownDraw = std::make_shared<DrawSpan>(ownSpans->draws()[0]);
    auto kept = single(only, ownDraw);
    std::weak_ptr<Frame> life = only;
    std::weak_ptr<const DrawSpanState> stateLife = ownDraw->state;
    ownDraw.reset(); ownSpans.reset(); only.reset();
    CHECK(!life.expired() && !stateLife.expired());
    CHECK(kept->chunks()[0].remap == (std::vector<uint32_t>{0, 1, 2}));
    kept.reset(); CHECK(life.expired() && stateLife.expired());
}
} // namespace

int main() {
    static_assert(sizeof(uint16_t) == 2);
    static_assert(std::is_same_v<decltype(IndexChunk::indices)::value_type, uint16_t>);
    pointBoundaries();
    wholeLinesAndTriangles();
    sparseRepeatedAndUploadWords();
    expandedTopologyAndDeterminism();
    eventOrderAndLifetime();
    malformedTransactions();
    printf("gx_index_chunks: %zu checks passed (U16 remaps, whole primitives, sparse/degenerate/order/provenance, lifetime, transactional bounds)\n", checks);
}
