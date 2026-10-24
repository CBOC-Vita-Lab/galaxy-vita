#include <stddef.h>
#include <revolution/tpl.h>
#include "JSystem/J3DGraphLoader/J3DShapeFactory.hpp"
#include "JSystem/JUtility/JUTTexture.hpp"
#include "JSystem/JAudio2/JASBNKParser.hpp"
#include "nw4r/lyt/resources.h"
#include "nw4r/ut/Font.h"
#include "Game/Screen/CinemaFrame.hpp"
#include <type_traits>

// The handheld must inherit the original layout draw, not the VR bars filter.
static_assert(std::is_same_v<decltype(&CinemaFrame::draw), void (LayoutActor::*)() const>,
              "Vita CinemaFrame must retain the original LayoutActor draw slot");

// These are actual decomp records, not replacement asset structures.
static_assert(sizeof(GXTexWrapMode) == 4);
static_assert(sizeof(GXTexFilter) == 4);
static_assert(sizeof(GXTlutFmt) == 4);
static_assert(sizeof(GXAttr) == 4);
static_assert(sizeof(GXCompCnt) == 4);
static_assert(sizeof(GXCompType) == 4);
static_assert(sizeof(TPLHeader) == 36);
static_assert(offsetof(TPLHeader, wrapS) == 12);
static_assert(offsetof(TPLHeader, wrapT) == 16);
static_assert(offsetof(TPLHeader, minFilter) == 20);
static_assert(offsetof(TPLHeader, magFilter) == 24);
static_assert(offsetof(TPLHeader, LODBias) == 28);
static_assert(sizeof(TPLClutHeader) == 12);
static_assert(offsetof(TPLClutHeader, data) == 8);
static_assert(sizeof(GXVtxAttrFmtList) == 16);
static_assert(offsetof(GXVtxAttrFmtList, frac) == 12);
static_assert(sizeof(GXVtxDescList) == 8);
static_assert(sizeof(ResTIMG) == 0x20);
static_assert(offsetof(ResTIMG, mImageDataOffset) == 0x1c);
static_assert(sizeof(J3DShapeInitData) == 0x28);
static_assert(offsetof(J3DShapeInitData, mRadius) == 0x0c);
static_assert(sizeof(J3DJointBlock) == 0x18);
static_assert(offsetof(J3DJointBlock, mpJointInitData) == 0x0c);
static_assert(sizeof(nw4r::lyt::res::TexMap) == 4);
static_assert(sizeof(nw4r::lyt::res::Layout) == 20);
static_assert(sizeof(nw4r::ut::FontTextureGlyph) == 24);
static_assert(offsetof(nw4r::ut::FontTextureGlyph, sheetImage) == 20);
static_assert(sizeof(nw4r::ut::FontInformation) == 24);
static_assert(offsetof(nw4r::ut::FontInformation, pGlyph) == 8);
static_assert(sizeof(nw4r::ut::Glyph) == 20);
static_assert(offsetof(nw4r::ut::Glyph, texWidth) == 12);
static_assert(sizeof(JASBNKParser::TFileHeader) == 16);
static_assert(sizeof(JASBNKParser::Ver1::TOsc) == 28);
static_assert(offsetof(JASBNKParser::Ver1::TOsc, mTableOffset) == 12);
static_assert(sizeof(JASBNKParser::Ver0::TOsc) == 24);
static_assert(sizeof(JASOscillator::Point) == 6);
static_assert(sizeof(JASOscillator::Data) == 24);
static_assert(offsetof(JASOscillator::Data, mTable) == 8);
