// Dedicated programs for the back end's copy quad (GXV_COPYVAR).
//
// The back end draws one program for every EFB copy, every resample, every restore of a colour buffer, the loading
// picture and the progress bar: out = u_mat * texel + u_add (the copy transform of the GX format: identity for the
// colour formats, the luma weights for the intensity formats, a channel for R8/G8/B8/A8, 0 * texel + colour for a solid
// fill). Three forms of that transform are by far the commonest in the draws that go through it and do not need the
// matrix at all:
//
//   Identity    u_mat = I, u_add = 0         out = texel                    (RGBA8 / RGB5A3 copies; the resamples,
//                                                                             the EFB restores, the hoisted and pass copies)
//   RgbConstA   u_mat = I with m33 = 0,      out = (texel.rgb, u_add.a)     (RGB565; any colour format of an EFB with no
//               u_add = (0, 0, 0, a)                                          alpha plane: alpha reads as 1)
//   Constant    u_mat = 0                    out = u_add                    (solid fills: the progress bar, the white
//                                                                             quad of a colour-only clear, the picture's frame)
//
// Each is the same arithmetic with the terms that are multiplied by 0 or 1 left out, so the pixels are identical (0 * x
// is 0 and 1 * x is x for every finite x, and the textures are 8-bit normalised); a form that does not match exactly is
// drawn by the generic program as before. The variants have fewer uniforms (u_rect only; u_rect and u_add; u_add only)
// and no mat4 product: two uniform calls and 80 bytes of the uniform ring fewer for an identity copy, the sampler as
// well for a constant. Intensity and single-channel forms are NOT specialised: their sums would be formed in another
// order than the generic program's, which is a rounding difference at the last bit of a float and so not "the same".
//
// GXV_COPYVAR=1 (default 0, read once by the back end). The sources below are the ones the back end compiles and the
// host test (tests/vgx_copyvar_gl.cpp) renders against the generic program.
#pragma once
#include <cstdint>

namespace vgx::copyvar {

enum Form : uint8_t { kGeneric = 0, kIdentity, kRgbConstA, kConstant, kForms };

// m16: column-major (m16[col * 4 + row], the order glUniformMatrix4fv takes); add: the four additive terms.
inline Form Classify(const float* m16, const float* add4) {
    bool diagonalOk = true;   // m = I on the 3 colour rows, columns 0..2, and zero elsewhere but m33
    bool identityOk = true;
    bool allZero = true;
    for (int i = 0; i < 16; ++i) {
        const float v = m16[i];
        if (v != 0.0f) allZero = false;
        const bool diag = (i % 5) == 0;
        if (!(v == (diag ? 1.0f : 0.0f))) identityOk = false;
        if (i != 15 && !(v == (diag ? 1.0f : 0.0f))) diagonalOk = false;
    }
    if (allZero) return kConstant;
    const bool addColourZero = add4[0] == 0.0f && add4[1] == 0.0f && add4[2] == 0.0f;
    if (identityOk && addColourZero && add4[3] == 0.0f) return kIdentity;
    if (diagonalOk && m16[15] == 0.0f && addColourZero) return kRgbConstA;
    return kGeneric;
}

// ---- the sources (GLSL ES 1.00)
// The vertex program is the generic one's, unchanged (so the vertex program of the shader cache is shared).
inline constexpr const char* kVertex =
    "attribute vec4 a_pos;\nuniform vec4 u_rect;\nvarying vec2 v_uv;\n"
    "void main() { v_uv = u_rect.xy + (a_pos.xy * 0.5 + 0.5) * u_rect.zw; gl_Position = vec4(a_pos.xy, 0.0, 1.0); }\n";
inline constexpr const char* kFragmentGeneric =
    "precision mediump float;\nuniform sampler2D u_src;\nuniform mat4 u_mat;\nuniform vec4 u_add;\nvarying vec2 v_uv;\n"
    "void main() { gl_FragColor = u_mat * texture2D(u_src, v_uv) + u_add; }\n";
inline constexpr const char* kFragmentIdentity =
    "precision mediump float;\nuniform sampler2D u_src;\nvarying vec2 v_uv;\n"
    "void main() { gl_FragColor = texture2D(u_src, v_uv); }\n";
inline constexpr const char* kFragmentRgbConstA =
    "precision mediump float;\nuniform sampler2D u_src;\nuniform vec4 u_add;\nvarying vec2 v_uv;\n"
    "void main() { gl_FragColor = vec4(texture2D(u_src, v_uv).rgb, u_add.w); }\n";
inline constexpr const char* kFragmentConstant =
    "precision mediump float;\nuniform vec4 u_add;\nvarying vec2 v_uv;\n"
    "void main() { gl_FragColor = u_add; }\n";

inline const char* FragmentSource(Form f) {
    switch (f) {
    case kIdentity: return kFragmentIdentity;
    case kRgbConstA: return kFragmentRgbConstA;
    case kConstant: return kFragmentConstant;
    default: return kFragmentGeneric;
    }
}
inline const char* Name(Form f) {
    switch (f) {
    case kIdentity: return "identity";
    case kRgbConstA: return "rgb+constant alpha";
    case kConstant: return "constant";
    default: return "generic";
    }
}
// Which uniforms the form's program has (the sampler is always u_src for the first two).
inline bool UsesRect(Form f) { return f == kIdentity || f == kRgbConstA; }
inline bool UsesAdd(Form f) { return f == kRgbConstA || f == kConstant; }
inline bool UsesTexture(Form f) { return f != kConstant; }

} // namespace vgx::copyvar
