// The Wii system (IPL ROM) font is only used by the Home Button menu, which the
// port does not show; these keep nw4r::ut::RomFont's vtable complete.
#include "nw4r/ut/RomFont.h"

namespace nw4r {
    namespace ut {
        int RomFont::GetWidth() const { return 0; }
        int RomFont::GetHeight() const { return 0; }
        int RomFont::GetAscent() const { return 0; }
        int RomFont::GetDescent() const { return 0; }
        int RomFont::GetBaselinePos() const { return 0; }
        int RomFont::GetCellHeight() const { return 0; }
        int RomFont::GetCellWidth() const { return 0; }
        int RomFont::GetMaxCharWidth() const { return 0; }
        Font::Type RomFont::GetType() const { return TYPE_ROM; }
        GXTexFmt RomFont::GetTextureFormat() const { return GX_TF_I4; }
        int RomFont::GetLineFeed() const { return 0; }
        const CharWidths RomFont::GetDefaultCharWidths() const { return CharWidths(); }
        void RomFont::SetDefaultCharWidths(const CharWidths&) {}
        bool RomFont::SetAlternateChar(CharCode) { return false; }
        void RomFont::SetLineFeed(int) {}
        int RomFont::GetCharWidth(CharCode) const { return 0; }
        const CharWidths RomFont::GetCharWidths(CharCode) const { return CharWidths(); }
        void RomFont::GetGlyph(Glyph*, CharCode) const {}
        bool RomFont::HasGlyph(CharCode) const { return false; }
        FontEncoding RomFont::GetEncoding() const { return FONT_ENCODING_UTF16; }
    }  // namespace ut
}  // namespace nw4r
