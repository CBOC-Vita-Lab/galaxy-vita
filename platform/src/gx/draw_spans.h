// Immutable, graphics-API-independent replay of a published GX recording.
#pragma once

#include <stddef.h>

#include <memory>
#include <string>
#include <vector>

#include "gx_record.h"
#include "xf_state.h"

namespace gpu {

enum class SpanTopology : uint32_t { Triangles, Lines, Points };
enum class SpanBlendFactor : uint32_t {
    Zero, One, SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor,
    SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha
};
enum class SpanBlendEquation : uint32_t { Add, ReverseSubtract };
enum class SpanDepthFunc : uint32_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class SpanCull : uint32_t { None, Front, Back, Both };

struct SpanMarkers {
    bool hud = false, sky = false, player = false, pointer = false;
};

struct SpanCamera {
    bool valid = false;
    uint32_t words[kCameraWords]{};
};

struct SpanRaster {
    float viewport[4]{};  // x, y, width, height; native EFB, top-left origin
    int32_t scissor[4]{}; // left, top, right-exclusive, bottom-exclusive
    uint32_t colorMask = 0; // R/G/B/A in bits 0/1/2/3
    bool blend = false, dstAlpha = false, depthTest = false, depthWrite = false;
    SpanBlendFactor src = SpanBlendFactor::Zero, dst = SpanBlendFactor::Zero;
    SpanBlendEquation equation = SpanBlendEquation::Add;
    SpanDepthFunc depthFunc = SpanDepthFunc::Never;
    SpanCull cull = SpanCull::None;
    float dstAlphaValue = 0;
};

enum class SpanTextureKind : uint32_t { Unbound, Image, EfbAddress };
struct SpanTexture {
    SpanTextureKind kind = SpanTextureKind::Unbound;
    uint32_t imageIndex = ~0u, copyId = 0, address = 0;
    uint32_t width = 0, height = 0, levels = 0;
    uint64_t version = 0;
    uint32_t mode0 = 0, mode1 = 0; // retained even for shader-unused units
};

// Raw registers preserve contracts not yet implemented by the GL baseline
// (for example fog range, logic operations and dither). No emulated RAM pointers.
struct SpanRegisters {
    uint32_t bp[256]{}, xf[256]{}, tevReg[8]{}, tevKonst[8]{};
};

struct DrawSpanState {
    ResolvedXf xf;
    PixelState pixel{};
    ShaderUid uid{}; // ordinary flat-screen key; no VR screen-texgen replacement
    SpanRaster raster;
    SpanTexture textures[8];
    uint32_t usedTextureMask = 0;
    SpanMarkers markers;
    SpanCamera camera; // first camera of this frame, matching Galaxy Builder
    SpanRegisters registers;
    uint32_t efbWidth = 0, efbHeight = 0;
};

struct DrawSpan {
    size_t commandWord = 0;
    uint32_t primitive = 0, vtxFlags = 0, firstWord = 0, vertexCount = 0, strideWords = 0;
    SpanTopology topology = SpanTopology::Points;
    std::shared_ptr<const DrawSpanState> state;
    std::vector<uint32_t> indices; // local to this draw; never narrowed to 16 bits
};

struct SpanCopy {
    uint32_t words[10]{}; // verbatim CMD_EFB_COPY payload, including copy ID
    uint32_t pixelFormat = 0, colorMode = 0, depthMode = 0;
    uint32_t filter[2]{}; // BP 0x53/0x54, copy-time vertical filter coefficients
    uint32_t efbWidth = 0, efbHeight = 0; // dimensions before this copy
    SpanMarkers markers;
};

struct SpanMarker {
    uint32_t kind = 0;
    SpanMarkers after;
};

enum class SpanEventKind : uint32_t { Draw, Copy, Marker, Camera };
struct SpanEvent {
    SpanEventKind kind;
    size_t index; // index in the corresponding typed vector
    size_t commandWord;
};

struct DrawSpanBuildOptions {
    // Carry the preceding result's terminal dimensions into the next frame.
    uint32_t efbWidth = 640, efbHeight = 456;
};

struct DrawSpanError {
    size_t commandWord = 0;
    uint32_t opcode = 0;
    std::string message;
};

class DrawSpans {
public:
    const Frame& frame() const { return *mFrame; }
    const std::shared_ptr<const Frame>& frameOwner() const { return mFrame; }
    const std::vector<DrawSpan>& draws() const { return mDraws; }
    const std::vector<SpanCopy>& copies() const { return mCopies; }
    const std::vector<SpanMarker>& markers() const { return mMarkers; }
    const std::vector<SpanCamera>& cameras() const { return mCameras; }
    const std::vector<SpanEvent>& events() const { return mEvents; }
    uint32_t efbWidth() const { return mWidth; }
    uint32_t efbHeight() const { return mHeight; }
    uint32_t clearAR() const { return mClearAR; }
    uint32_t clearGB() const { return mClearGB; }
    uint32_t clearZ() const { return mClearZ; }

private:
    DrawSpans() = default;
    friend bool buildDrawSpans(std::shared_ptr<const Frame>, std::shared_ptr<const DrawSpans>&,
                              DrawSpanError&, DrawSpanBuildOptions);
    std::shared_ptr<const Frame> mFrame;
    std::vector<DrawSpan> mDraws;
    std::vector<SpanCopy> mCopies;
    std::vector<SpanMarker> mMarkers;
    std::vector<SpanCamera> mCameras;
    std::vector<SpanEvent> mEvents;
    uint32_t mWidth = 0, mHeight = 0, mClearAR = 0, mClearGB = 0, mClearZ = 0;
};

// Transactional: on malformed input returns false, clears output, and sets a
// word offset/opcode/reason. Caller must report the error, not submit a partial
// frame. Published Frame vertices and TexImage pixel payloads must stay frozen.
[[nodiscard]] bool buildDrawSpans(std::shared_ptr<const Frame> frame,
                                 std::shared_ptr<const DrawSpans>& output,
                                 DrawSpanError& error, DrawSpanBuildOptions options = {});

} // namespace gpu
