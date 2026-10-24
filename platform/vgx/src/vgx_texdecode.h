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
// A plain range-fit DXT1 encoder for opaque RGBA8 (mip levels below the authored ones).
void EncodeDxt1(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t* dst);
// 2x2 box filter, for completing mip chains.
void Downsample(const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& dst, uint32_t& dw, uint32_t& dh);
}
