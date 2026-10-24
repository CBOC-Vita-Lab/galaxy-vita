// Structural shader keys and GLSL ES 1.00 generation. Front-end side: no GL.
#pragma once
#include "vgx_cmd.h"

namespace vgx {

// Everything that changes the *code* of a shader goes into a key; everything
// that is only a value (colours, matrices, references, fog parameters) is a
// uniform. Keys are plain zero-initialised structs compared with memcmp.
struct FsKey {
    uint8_t numStages = 1;    // 1..16
    uint8_t numTexGens = 0;   // texcoord varyings declared
    uint8_t texProjMask = 0;  // texcoord k is projective (divide by q)
    uint8_t alphaTestOn = 0;
    uint8_t alphaTest = 0;    // comp0 | comp1 << 3 | op << 6
    uint8_t fog = 0;          // fsel | ortho << 3 (0 = off)
    uint8_t dstAlpha = 0;     // write the constant destination alpha
    uint8_t numIndStages = 0;
    uint8_t ztex = 0;         // op << 2 | fmt (0 = off)
    // TEV registers 0 and 1 start from the vertex colours instead of u_tev[1] / u_tev[2]
    // (bit 0, bit 1). Set for small draws that do not use the rasterised colours at all:
    // the front end writes the register values into the vertices, so that draws which
    // differ only in them (particles fading out) merge into one. See FoldRegs in vgx_draw.cpp.
    uint8_t regFromVtx = 0;
    uint8_t pad[2] = {};
    struct Stage {
        uint32_t color = 0; // BP 0xC0+2n bits 0..23
        uint32_t alpha = 0; // BP 0xC1+2n bits 4..23 (swap selects stripped)
        uint32_t ind = 0;   // BP 0x10+n (0 when the stage is direct)
        uint8_t texEnable = 0, texMap = 0, texCoord = 0, rasChan = 7; // 0, 1, 5/6 (bump alpha), 7 = zero
        uint8_t kcsel = 0, kasel = 0;
        uint8_t rasSwap = 0xE4, texSwap = 0xE4; // 2 bits per output component, r in bits 0..1
        uint8_t pad[4] = {};
    } stage[16];
    struct Ind {
        uint8_t texMap = 0, texCoord = 0, pad[2] = {};
    } ind[4];
};

struct VsKey {
    uint8_t numTexGens = 0;
    uint8_t numChans = 0;   // 0..2 lit colour channels produced
    uint8_t perVtxPos = 0;  // position/normal matrix row comes from a_pos.w (2: no row above 27 is used, the palette is the short one)
    uint8_t pad = 0;
    // Per channel control: [0] colour0, [1] colour1, [2] alpha0, [3] alpha1.
    struct Chan {
        uint8_t enable = 0, matSrcVtx = 0, ambSrcVtx = 0;
        uint8_t diffuse = 0; // 0 none, 1 sign, 2 clamp
        uint8_t attn = 0;    // 0 none, 1 spec, 2 spot
        uint8_t lightMask = 0;
        uint8_t pad[2] = {};
    } chan[4];
    struct TexGen {
        uint8_t type = 0;      // 0 regular, 1 emboss, 2 colour0, 3 colour1
        uint8_t src = 5;       // XF source row: 0 pos, 1 nrm, 2 colours, 3 binrm, 4 tangent, 5.. tex0..
        uint8_t proj = 0;      // 1 = STQ (3x4 matrix, projective)
        uint8_t inputAbc1 = 0; // input form (a,b,c,1) instead of (a,b,1,1)
        uint8_t post = 0;      // 0 none, 1 post matrix, 2 normalise + post matrix
        uint8_t perVtxMtx = 0; // matrix row from the per-vertex TEXnMTXIDX
        uint8_t pad[2] = {};
    } tg[8];
};

uint64_t HashKey(const void* key, size_t size);
// Bump when the key structs or what is generated from them change: shader
// warm-up lists written by an older generator are then discarded.
constexpr uint32_t kShaderGenVersion = 2;
// Uniform arrays are declared (and their blocks built) only as long as the shader needs
// them: u_tev[9 / 11 / 28], u_lt[4 + 5 per light], u_pal[60 / 96], u_tm per texgen. On the
// console every draw copies the program's whole uniform buffers once more.
//   GXV_UNIPACK=0   the old fixed sizes (what the reference frame hashes were made with)
bool UniformPacking();
// Sizes in vec4 of what a shader with this key declares.
int FsTevCount(const FsKey& k);  // u_tev
int VsLightCount(const VsKey& k); // u_lt
constexpr int kPalRowsFull = 96, kPalRowsShort = 60, kPalShortMaxRow = 27;
void GenerateFragmentShader(const FsKey& key, ShaderSource& out);
void GenerateVertexShader(const VsKey& key, ShaderSource& out);

} // namespace vgx
