#include "draw_spans.h"

#include <cmath>
#include <cstring>
#include <utility>

#include "gpu.h"

namespace gpu {

namespace {

constexpr uint32_t kMode0[8] = {0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3};
constexpr uint32_t kImage0[8] = {0x88, 0x89, 0x8A, 0x8B, 0xA8, 0xA9, 0xAA, 0xAB};

float asFloat(uint32_t word) {
    float value;
    static_assert(sizeof(value) == sizeof(word));
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

void color(uint32_t rgba, float* out) {
    for (unsigned k = 0; k < 4; ++k) out[k] = ((rgba >> (24 - k * 8)) & 255) / 255.0f;
}

int signed11(uint32_t word) {
    int value = int(word & 0x7FF);
    return value & 0x400 ? value - 0x800 : value;
}

// Galaxy Builder::drawRecord/fillLights/fillProj, without SSBO slot addresses.
void resolveXf(const SpanRegisters& regs, const float* mem, uint32_t flags, ResolvedXf& out) {
    std::memcpy(out.pos, mem, sizeof(out.pos));
    for (unsigned r = 0; r < 33; ++r) {
        for (unsigned k = 0; k < 3; ++k) out.normal[r][k] = mem[0x400 + r * 3 + k];
        out.normal[r][3] = 0;
    }
    // All eight selected post matrices are retained, including shader-dead
    // texgens. Start row 63 intentionally includes the two following rows.
    for (unsigned t = 0; t < 8; ++t) {
        unsigned start = regs.xf[0x50 + t] & 63;
        std::memcpy(out.post + t * 3, mem + 0x500 + start * 4, 12 * sizeof(float));
    }
    for (unsigned l = 0; l < 8; ++l) {
        const float* m = mem + 0x600 + l * 16;
        uint32_t rgba;
        std::memcpy(&rgba, m + 3, sizeof(rgba));
        color(rgba, out.lights.lights[l][0]);
        for (unsigned v = 1; v < 5; ++v) {
            for (unsigned k = 0; k < 3; ++k) out.lights.lights[l][v][k] = m[1 + v * 3 + k];
            out.lights.lights[l][v][3] = 0;
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        color(regs.xf[0x0C + i], out.lights.matColor[i]);
        color(regs.xf[0x0A + i], out.lights.ambColor[i]);
    }
    float p[6];
    for (unsigned i = 0; i < 6; ++i) p[i] = asFloat(regs.xf[0x20 + i]);
    auto& projection = out.projection;
    std::memset(&projection, 0, sizeof(projection));
    if (regs.xf[0x26] & 1) {
        projection.proj[0][0] = p[0]; projection.proj[0][3] = p[1];
        projection.proj[1][1] = p[2]; projection.proj[1][3] = p[3];
        projection.proj[2][2] = p[4]; projection.proj[2][3] = p[5];
        projection.proj[3][3] = 1;
    } else {
        projection.proj[0][0] = p[0]; projection.proj[0][2] = p[1];
        projection.proj[1][1] = p[2]; projection.proj[1][2] = p[3];
        projection.proj[2][2] = p[4]; projection.proj[2][3] = p[5];
        projection.proj[3][2] = -1;
    }
    projection.depthParams[0] = asFloat(regs.xf[0x1C]);
    projection.depthParams[1] = asFloat(regs.xf[0x1F]);
    out.matrixA = regs.xf[0x18];
    out.matrixB = regs.xf[0x19];
    out.flags = flags;
}

// Galaxy Builder::fillPix. Raw BP registers accompany the decoded baseline.
void resolvePixel(const SpanRegisters& regs, uint32_t width, uint32_t height, PixelState& out) {
    const uint32_t* bp = regs.bp;
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t lo = regs.tevReg[i * 2], hi = regs.tevReg[i * 2 + 1];
        out.tevReg[i][0] = signed11(lo) / 255.0f;
        out.tevReg[i][1] = signed11(hi >> 12) / 255.0f;
        out.tevReg[i][2] = signed11(hi) / 255.0f;
        out.tevReg[i][3] = signed11(lo >> 12) / 255.0f;
        lo = regs.tevKonst[i * 2]; hi = regs.tevKonst[i * 2 + 1];
        out.konst[i][0] = (lo & 255) / 255.0f;
        out.konst[i][1] = ((hi >> 12) & 255) / 255.0f;
        out.konst[i][2] = (hi & 255) / 255.0f;
        out.konst[i][3] = ((lo >> 12) & 255) / 255.0f;
    }
    out.alphaRef[0] = float(bp[0xF3] & 255);
    out.alphaRef[1] = float((bp[0xF3] >> 8) & 255);
    out.alphaRef[2] = float(bp[0xF4] & 0xFFFFFF);
    out.alphaRef[3] = 0;
    out.fogColor[0] = ((bp[0xF2] >> 16) & 255) / 255.0f;
    out.fogColor[1] = ((bp[0xF2] >> 8) & 255) / 255.0f;
    out.fogColor[2] = (bp[0xF2] & 255) / 255.0f;
    out.fogColor[3] = 1;
    out.fogParams[0] = asFloat((bp[0xEE] & 0xFFFFF) << 12);
    out.fogParams[1] = asFloat((bp[0xF1] & 0xFFFFF) << 12);
    out.fogParams[2] = float(bp[0xEF] & 0xFFFFFF);
    out.fogParams[3] = float(bp[0xF0] & 31);
    std::memset(out.fogRange, 0, sizeof(out.fogRange));
    for (unsigned t = 0; t < 8; ++t) {
        uint32_t img = bp[kImage0[t]];
        float w = float((img & 0x3FF) + 1), h = float(((img >> 10) & 0x3FF) + 1);
        int bias = int((bp[kMode0[t]] >> 9) & 255);
        out.texSize[t][0] = (bias >= 128 ? bias - 256 : bias) / 32.0f;
        out.texSize[t][1] = h;
        out.texSize[t][2] = 1 / w;
        out.texSize[t][3] = 1 / h;
    }
    for (unsigned m = 0; m < 3; ++m) {
        uint32_t ra = bp[0x06 + m * 3], rb = bp[0x07 + m * 3], rc = bp[0x08 + m * 3];
        int exponent = int(((ra >> 22) & 3) | (((rb >> 22) & 3) << 2) | (((rc >> 22) & 1) << 4)) - 17;
        float scale = std::ldexp(1.0f, exponent) / 1024.0f;
        uint32_t columns[3] = {ra, rb, rc};
        for (unsigned r = 0; r < 2; ++r) {
            for (unsigned k = 0; k < 3; ++k) out.indMtx[m][r][k] = signed11(columns[k] >> (r * 11)) * scale;
            out.indMtx[m][r][3] = 0;
        }
    }
    out.efbSize[0] = float(width); out.efbSize[1] = float(height);
    out.efbSize[2] = out.efbSize[3] = 0;
}

void resolveRaster(const SpanRegisters& regs, SpanRaster& out) {
    const uint32_t* bp = regs.bp;
    const uint32_t* xf = regs.xf;
    float sx = std::fabs(asFloat(xf[0x1A])), sy = std::fabs(asFloat(xf[0x1B]));
    int32_t xoff = int32_t((bp[0x59] & 0x3FF) * 2);
    int32_t yoff = int32_t(((bp[0x59] >> 10) & 0x3FF) * 2);
    out.viewport[0] = asFloat(xf[0x1D]) - sx - float(xoff);
    out.viewport[1] = asFloat(xf[0x1E]) - sy - float(yoff);
    out.viewport[2] = sx * 2; out.viewport[3] = sy * 2;
    out.scissor[0] = int32_t((bp[0x20] >> 12) & 0x7FF) - xoff;
    out.scissor[1] = int32_t(bp[0x20] & 0x7FF) - yoff;
    out.scissor[2] = int32_t((bp[0x21] >> 12) & 0x7FF) - xoff + 1;
    out.scissor[3] = int32_t(bp[0x21] & 0x7FF) - yoff + 1;
    uint32_t cm = bp[0x41];
    out.colorMask = ((cm & 8) ? 7 : 0) | ((cm & 16) ? 8 : 0);
    if ((bp[0x43] & 7) != 1) out.colorMask &= 7;
    out.blend = (cm & 1) != 0;
    constexpr SpanBlendFactor src[8] = {
        SpanBlendFactor::Zero, SpanBlendFactor::One, SpanBlendFactor::DstColor,
        SpanBlendFactor::OneMinusDstColor, SpanBlendFactor::SrcAlpha,
        SpanBlendFactor::OneMinusSrcAlpha, SpanBlendFactor::DstAlpha, SpanBlendFactor::OneMinusDstAlpha
    };
    constexpr SpanBlendFactor dst[8] = {
        SpanBlendFactor::Zero, SpanBlendFactor::One, SpanBlendFactor::SrcColor,
        SpanBlendFactor::OneMinusSrcColor, SpanBlendFactor::SrcAlpha,
        SpanBlendFactor::OneMinusSrcAlpha, SpanBlendFactor::DstAlpha, SpanBlendFactor::OneMinusDstAlpha
    };
    if (cm & (1 << 11)) {
        out.blend = true;
        out.src = out.dst = SpanBlendFactor::One;
        out.equation = SpanBlendEquation::ReverseSubtract;
    } else {
        out.src = src[(cm >> 8) & 7]; out.dst = dst[(cm >> 5) & 7];
        out.equation = SpanBlendEquation::Add;
    }
    out.dstAlpha = (bp[0x42] & 256) != 0;
    out.dstAlphaValue = (bp[0x42] & 255) / 255.0f;
    out.depthTest = (bp[0x40] & 1) != 0;
    out.depthWrite = (bp[0x40] & 16) != 0;
    out.depthFunc = static_cast<SpanDepthFunc>((bp[0x40] >> 1) & 7);
    out.cull = static_cast<SpanCull>((bp[0] >> 14) & 3);
}

uint32_t usedTextures(const uint32_t* bp) {
    uint32_t mask = 0;
    unsigned tevCount = ((bp[0] >> 10) & 15) + 1;
    for (unsigned s = 0; s < tevCount; ++s) {
        uint32_t order = bp[0x28 + s / 2] >> ((s & 1) * 12);
        if (order & 64) mask |= 1u << (order & 7);
    }
    unsigned indCount = (bp[0] >> 16) & 7;
    for (unsigned s = 0; s < indCount; ++s) mask |= 1u << ((bp[0x27] >> (s * 6)) & 7);
    return mask;
}

bool indices(uint32_t kind, uint32_t count, DrawSpan& out) {
    uint64_t size = 0;
    out.topology = kind <= 0xA0 ? SpanTopology::Triangles : kind <= 0xB0 ? SpanTopology::Lines : SpanTopology::Points;
    switch (kind) {
    case 0x80: case 0x88: size = uint64_t(count / 4) * 6; break;
    case 0x90: size = count / 3 * 3; break;
    case 0x98: case 0xA0: size = count >= 3 ? uint64_t(count - 2) * 3 : 0; break;
    case 0xA8: size = count / 2 * 2; break;
    case 0xB0: size = count >= 2 ? uint64_t(count - 1) * 2 : 0; break;
    case 0xB8: size = count; break;
    default: return false;
    }
    if (size > out.indices.max_size()) return false;
    out.indices.reserve(static_cast<size_t>(size));
    auto add = [&](uint32_t a, uint32_t b, uint32_t c) {
        out.indices.push_back(a); out.indices.push_back(b); out.indices.push_back(c);
    };
    switch (kind) {
    case 0x80: case 0x88:
        for (uint32_t i = 0; i < count / 4; ++i) {
            uint32_t v = i * 4; add(v, v + 1, v + 2); add(v, v + 2, v + 3);
        }
        break;
    case 0x98:
        for (uint32_t i = 0; i + 2 < count; ++i) add(i + (i & 1), i + 1 - (i & 1), i + 2);
        break;
    case 0xA0:
        for (uint32_t i = 1; i + 1 < count; ++i) add(0, i, i + 1);
        break;
    case 0xB0:
        for (uint32_t i = 0; i + 1 < count; ++i) { out.indices.push_back(i); out.indices.push_back(i + 1); }
        break;
    default:
        for (uint32_t i = 0; i < size; ++i) out.indices.push_back(i);
        break;
    }
    return true;
}

} // namespace

bool buildDrawSpans(std::shared_ptr<const Frame> frame, std::shared_ptr<const DrawSpans>& output,
                    DrawSpanError& error, DrawSpanBuildOptions options) {
    output.reset();
    error = {};
    size_t at = 0, cursor = 0;
    uint32_t opcode = 0;
    auto fail = [&](const char* message) {
        error = {at, opcode, message};
        return false;
    };
    if (!frame) return fail("null frame");
    if (!options.efbWidth || !options.efbHeight || options.efbWidth > 1024 || options.efbHeight > 1024)
        return fail("initial EFB dimensions must be within 1..1024");
    auto result = std::shared_ptr<DrawSpans>(new DrawSpans);
    result->mFrame = std::move(frame);
    const Frame& f = result->frame();
    result->mWidth = options.efbWidth; result->mHeight = options.efbHeight;
    result->mClearAR = f.startBp[0x4F]; result->mClearGB = f.startBp[0x50];
    result->mClearZ = f.startBp[0x51] & 0xFFFFFF;
    SpanRegisters regs;
    std::memcpy(regs.bp, f.startBp, sizeof(regs.bp));
    std::memcpy(regs.xf, f.startXfRegs, sizeof(regs.xf));
    std::memcpy(regs.tevReg, f.startTevReg, sizeof(regs.tevReg));
    std::memcpy(regs.tevKonst, f.startTevKonst, sizeof(regs.tevKonst));
    float xfMem[0x1000];
    std::memcpy(xfMem, f.startXfMem, sizeof(xfMem));
    if (f.startMarkers & ~15u) return fail("invalid initial marker state");
    SpanMarkers markers{bool(f.startMarkers&1),bool(f.startMarkers&2),bool(f.startMarkers&4),bool(f.startMarkers&8)};
    SpanCamera camera;
    camera.valid=f.startCameraValid;
    if (camera.valid) std::memcpy(camera.words,f.startCamera,sizeof(camera.words));
    SpanTexture textures[8];
    std::shared_ptr<const DrawSpanState> state;
    auto available = [&](size_t words) { return words <= f.cmds.size() - cursor; };
    auto event = [&](SpanEventKind kind, size_t index) { result->mEvents.push_back({kind, index, at}); };
    while (cursor < f.cmds.size()) {
        at = cursor;
        opcode = f.cmds[cursor++];
        // Only adjacent, untouched draw state is shared. Every other command
        // invalidates the entire snapshot; no field-specific dirty heuristics.
        if (opcode != CMD_DRAW) state.reset();
        switch (opcode) {
        case CMD_XF: {
            if (!available(2)) return fail("truncated XF header");
            uint32_t address = f.cmds[cursor++], count = f.cmds[cursor++];
            if (!available(count)) return fail("truncated XF values");
            if (!count || address >= 0x1100 || count > 0x1100 - address)
                return fail("XF write outside memory/register range or empty write");
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t a = address + i, value = f.cmds[cursor++];
                if (a < 0x1000) xfMem[a] = asFloat(value);
                else regs.xf[a - 0x1000] = value;
            }
            break;
        }
        case CMD_BP: {
            if (!available(1)) return fail("truncated BP write");
            uint32_t packed = f.cmds[cursor++], reg = packed >> 24, value = packed & 0xFFFFFF;
            regs.bp[reg] = value;
            trackTevColorWrite(regs.tevReg, regs.tevKonst, reg, value);
            break;
        }
        case CMD_TEX: {
            if (!available(2)) return fail("truncated texture binding");
            uint32_t unit = f.cmds[cursor++], index = f.cmds[cursor++];
            if (unit >= 8) return fail("texture unit outside 0..7");
            SpanTexture binding;
            if (index != ~0u) {
                if (index >= f.textures.size() || !f.textures[index]) return fail("invalid texture image index");
                const TexImage& image = *f.textures[index];
                if (!image.width || !image.height || !image.levels) return fail("texture image has empty dimensions or mip count");
                binding.kind = SpanTextureKind::Image; binding.imageIndex = index;
                binding.width = image.width; binding.height = image.height;
                binding.levels = image.levels; binding.version = image.version;
            }
            textures[unit] = binding;
            break;
        }
        case CMD_TEX_EFB: {
            if (!available(5)) return fail("truncated EFB texture binding");
            const uint32_t* words = f.cmds.data() + cursor;
            if (words[0] >= 8) return fail("EFB texture unit outside 0..7");
            if (!words[2] || !words[3] || words[2] > 1024 || words[3] > 1024)
                return fail("EFB texture dimensions outside 1..1024");
            SpanTexture binding;
            binding.kind = SpanTextureKind::EfbAddress;
            binding.copyId = words[1]; binding.width = words[2]; binding.height = words[3];
            binding.address = words[4]; binding.levels = 1;
            textures[words[0]] = binding;
            cursor += 5;
            break;
        }
        case CMD_MARKER: {
            if (!available(1)) return fail("truncated marker");
            uint32_t kind = f.cmds[cursor++];
            switch (kind) {
            case MARK_HUD_BEGIN: markers.hud = true; break;
            case MARK_HUD_END: markers.hud = false; break;
            case MARK_SKY_BEGIN: markers.sky = true; break;
            case MARK_SKY_END: markers.sky = false; break;
            case MARK_PLAYER_BEGIN: markers.player = true; break;
            case MARK_PLAYER_END: markers.player = false; break;
            case MARK_POINTER_BEGIN: markers.pointer = true; break;
            case MARK_POINTER_END: markers.pointer = false; break;
            case MARK_SCENE_DEPTH: break;
            default: return fail("unknown marker kind");
            }
            event(SpanEventKind::Marker, result->mMarkers.size());
            result->mMarkers.push_back({kind, markers});
            break;
        }
        case CMD_CAMERA: {
            if (!available(kCameraWords)) return fail("truncated camera");
            SpanCamera incoming;
            incoming.valid = true;
            std::memcpy(incoming.words, f.cmds.data() + cursor, sizeof(incoming.words));
            cursor += kCameraWords;
            if (!camera.valid) camera = incoming;
            event(SpanEventKind::Camera, result->mCameras.size());
            result->mCameras.push_back(incoming);
            break;
        }
        case CMD_EFB_COPY: {
            if (!available(10)) return fail("truncated EFB copy");
            SpanCopy copy;
            std::memcpy(copy.words, f.cmds.data() + cursor, sizeof(copy.words));
            cursor += 10;
            copy.pixelFormat = regs.bp[0x43]; copy.colorMode = regs.bp[0x41]; copy.depthMode = regs.bp[0x40];
            copy.filter[0] = regs.bp[0x53]; copy.filter[1] = regs.bp[0x54];
            copy.efbWidth = result->mWidth; copy.efbHeight = result->mHeight; copy.markers = markers;
            event(SpanEventKind::Copy, result->mCopies.size());
            result->mCopies.push_back(copy);
            if (copy.words[9] & (1 << 14)) {
                result->mWidth = (copy.words[2] & 0x3FF) + 1;
                result->mHeight = ((copy.words[2] >> 10) & 0x3FF) + 1;
            }
            break;
        }
        case CMD_DRAW: {
            if (!available(4)) return fail("truncated draw");
            DrawSpan span;
            span.commandWord = at;
            span.primitive = f.cmds[cursor++]; span.vtxFlags = f.cmds[cursor++];
            span.firstWord = f.cmds[cursor++]; span.vertexCount = f.cmds[cursor++];
            if (span.primitive > 255) return fail("primitive is not a GX opcode byte");
            if ((span.vtxFlags & ~0x3FFFu) || ((span.vtxFlags & VF_NBT) && !(span.vtxFlags & VF_NRM)))
                return fail("invalid decoded vertex flags");
            span.strideWords = vtxWords(span.vtxFlags);
            uint64_t words = uint64_t(span.vertexCount) * span.strideWords;
            if (span.firstWord > f.verts.size() || words > f.verts.size() - span.firstWord)
                return fail("draw vertex range outside recorded words");
            if ((regs.xf[0x3F] & 15) > 8 || (regs.xf[0x09] & 3) > 2 || ((regs.bp[0] >> 16) & 7) > 4)
                return fail("GX texgen/channel/indirect count exceeds hardware limits");
            if (span.vertexCount && (span.vtxFlags & VF_NRM) && !(span.vtxFlags & VF_POSMTX) && (regs.xf[0x18] & 31) > 30)
                return fail("default normal matrix rows exceed resolved palette");
            if (span.vtxFlags & (VF_POSMTX | VF_TEXMTX)) {
                for (uint32_t i = 0; i < span.vertexCount; ++i) {
                    const uint32_t* vertex = f.verts.data() + span.firstWord + size_t(i) * span.strideWords;
                    if ((span.vtxFlags & VF_POSMTX) && vertex[3] > 63)
                        return fail("position matrix index outside 0..63");
                    if ((span.vtxFlags & VF_POSMTX) && (span.vtxFlags & VF_NRM) && (vertex[3] & 31) > 30)
                        return fail("normal matrix rows exceed resolved palette");
                    if (span.vtxFlags & VF_TEXMTX) {
                        for (unsigned t = 0; t < 8; ++t) {
                            uint32_t packed = vertex[span.strideWords - 2 + t / 4];
                            if (((packed >> ((t % 4) * 8)) & 255) > 63)
                                return fail("texture matrix index outside 0..63");
                        }
                    }
                }
            }
            if (!indices(span.primitive & 0xF8, span.vertexCount, span))
                return fail("unknown GX primitive or expanded index range too large");
            if (!state || state->uid.vtxFlags != span.vtxFlags) {
                auto next = std::make_shared<DrawSpanState>();
                next->registers = regs; next->markers = markers; next->camera = camera;
                next->efbWidth = result->mWidth; next->efbHeight = result->mHeight;
                next->uid = makeShaderUid(regs.bp, regs.xf, span.vtxFlags, markers.hud);
                resolveRaster(regs, next->raster);
                const float* vp = next->raster.viewport;
                bool eligible = !(regs.xf[0x26] & 1) && camera.valid && !markers.hud &&
                    vp[0] <= 1 && vp[1] <= 1 && vp[2] >= float(result->mWidth) - 2 && vp[3] >= float(result->mHeight) - 2;
                uint32_t flags = (eligible ? 1u : 0u) | (eligible && markers.sky ? 2u : 0u) | (markers.pointer ? 4u : 0u);
                resolveXf(regs, xfMem, flags, next->xf);
                resolvePixel(regs, result->mWidth, result->mHeight, next->pixel);
                next->usedTextureMask = usedTextures(regs.bp);
                for (unsigned t = 0; t < 8; ++t) {
                    next->textures[t] = textures[t];
                    next->textures[t].mode0 = regs.bp[kMode0[t]];
                    next->textures[t].mode1 = regs.bp[kMode0[t] + 4];
                }
                state = std::move(next);
            }
            span.state = state;
            event(SpanEventKind::Draw, result->mDraws.size());
            result->mDraws.push_back(std::move(span));
            break;
        }
        default: return fail("unknown recording opcode");
        }
    }
    output = std::move(result);
    return true;
}

} // namespace gpu
