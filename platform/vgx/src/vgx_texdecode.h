// GX texture formats -> RGBA8 (front end; pure CPU, no GL).
#pragma once
#include <cstdint>
#include <vector>

namespace vgx {
// Bytes occupied by one mip level of a GX texture (tiles are padded).
uint32_t TexLevelSize(uint32_t width, uint32_t height, uint32_t fmt);
// Total bytes of `levels` mip levels starting at level 0.
uint32_t TexDataSize(uint32_t width, uint32_t height, uint32_t fmt, uint32_t levels);
bool TexFormatIsCI(uint32_t fmt);
// Decodes one level into width*height RGBA8 pixels (R first), top row first.
// tlut/tlutFmt/tlutEntries are only read for the C4/C8/C14X2 formats.
void DecodeTexture(const uint8_t* src, uint32_t width, uint32_t height, uint32_t fmt, const uint8_t* tlut,
                   uint32_t tlutFmt, uint32_t tlutEntries, uint8_t* dstRGBA);
// (GXV_TEXDEC_FAST, for the host test) the reference decoder, and the whole-tile fast one: false = not taken (DecodeTexture then
// uses the reference); when taken its output is byte for byte the reference's.
namespace detail {
void DecodeTextureRef(const uint8_t* src, uint32_t width, uint32_t height, uint32_t fmt, const uint8_t* tlut, uint32_t tlutFmt,
                      uint32_t tlutEntries, uint8_t* dstRGBA);
bool DecodeTextureFast(const uint8_t* src, uint32_t width, uint32_t height, uint32_t fmt, const uint8_t* tlut, uint32_t tlutFmt,
                       uint32_t tlutEntries, uint8_t* dstRGBA);
}
// I4/I8/IA4/IA8 only: width*height LA88 pairs, sampling as (L,L,L,A).
// I4/I8 replicate intensity into alpha too. Invalid format/size/pointer returns false
// without writing; source and destination must hold a padded GX level and 2*w*h bytes.
bool DecodeTextureLA88(const uint8_t* src, uint32_t width, uint32_t height, uint32_t fmt, uint8_t* dstLA);
// CMPR is S3TC DXT1 in another byte and block order: converts one level (8x8 tiles of four
// big-endian sub-blocks) into ((w + 3) / 4) * ((h + 3) / 4) DXT1 blocks, row-major, as
// GL_COMPRESSED_RGBA_S3TC_DXT1_EXT takes them. Returns false (dst unusable) when the level
// has a three-colour block that uses its fourth entry: on the GameCube that texel is
// transparent with the colour of the block's average, in DXT1 it is transparent black, and
// the difference shows as dark fringes wherever such a texture is filtered. Those textures
// have to be decoded instead (allowTransparent: convert them all the same).
bool CmprToDxt1(const uint8_t* src, uint32_t width, uint32_t height, uint8_t* dst, bool allowTransparent);
inline uint32_t Dxt1Size(uint32_t width, uint32_t height) { return ((width + 3) / 4) * ((height + 3) / 4) * 8; }
// GXV_CMPRA (ported from the Strikers port's 1.3 MSC_VGX_CMPRA): one CMPR level as DXT5, for the levels CmprToDxt1 refuses: 8 bits per
// texel instead of the 32 of a decoded texture. Alpha is the same 0 or 255 per texel. Four-colour blocks keep their colours. A three-colour
// block's middle colour (also the colour of its transparent texels) is not a DXT5 palette entry: its texels alternate between the two inner
// entries in a checkerboard, whose mean is that colour; each is at most a sixth of c0 to c1 away.
void CmprToDxt5(const uint8_t* src, uint32_t width, uint32_t height, uint8_t* dst);
inline uint32_t Dxt5Size(uint32_t width, uint32_t height) { return ((width + 3) / 4) * ((height + 3) / 4) * 16; }
// GXV_TEX16 (the Strikers port's 1.3 MSC_VGX_TEX16): the 16-bit format (vgx_cmd.h TF_RGB565, TF_RGBA5551, TF_RGBA4444) that holds every
// texel of these RGBA8 levels without loss (each 8-bit value is the bit replica of its 5/6/4-bit value), or 0 for none; allow5551 = false
// leaves that format out. PackRgba16 rewrites one level as little-endian 16-bit words of that format.
uint8_t Rgba16Format(const std::vector<std::vector<uint8_t>>& levels, bool allow5551);
void PackRgba16(std::vector<uint8_t>& level, uint8_t format);
// A plain range-fit DXT1 encoder for opaque RGBA8 (mip levels below the authored ones).
void EncodeDxt1(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t* dst);
// 2x2 box filter, for completing mip chains.
void Downsample(const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& dst, uint32_t& dw, uint32_t& dh);
}
