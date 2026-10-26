#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <utility>
#include <type_traits>

namespace port::settings {
constexpr size_t kFontArchiveLimit = 32*1024*1024;
constexpr size_t kFontFileLimit = 16*1024*1024;
constexpr size_t kFontAlphaLimit = 16*1024*1024;
constexpr size_t kFontDecodeLimit = 8*1024*1024;
struct FontAllocator {
    void* context = nullptr;
    void* (*allocate)(void*, size_t) = [](void*, size_t bytes) { return std::malloc(bytes); };
    void (*release)(void*, void*) = [](void*, void* data) { std::free(data); };
};
// Optional font storage must not reach the target's fatal operator new path.
template<class T> class FontBuffer {
    static_assert(std::is_trivially_copyable<T>::value && std::is_trivially_destructible<T>::value, "Font storage requires POD values");
public:
    FontBuffer() = default;
    FontBuffer(const FontBuffer&) = delete;
    FontBuffer& operator=(const FontBuffer&) = delete;
    FontBuffer(FontBuffer&& other) noexcept { *this = std::move(other); }
    FontBuffer& operator=(FontBuffer&& other) noexcept {
        if (this != &other) {
            Reset(); mData = std::exchange(other.mData, nullptr);
            mSize = std::exchange(other.mSize, 0); mAllocator = other.mAllocator;
        }
        return *this;
    }
    ~FontBuffer() { Reset(); }
    bool Allocate(size_t count, const FontAllocator& allocator) {
        Reset();
        if (!count || count > SIZE_MAX / sizeof(T) || !allocator.allocate || !allocator.release) return false;
        mData = static_cast<T*>(allocator.allocate(allocator.context, count * sizeof(T)));
        if (!mData) return false;
        mSize = count; mAllocator = allocator;
        for (size_t i = 0; i < count; ++i) ::new (static_cast<void*>(mData + i)) T{};
        return true;
    }
    size_t size() const { return mSize; }
    bool empty() const { return !mSize; }
    T* begin() { return mData; }
    T* end() { return mData ? mData + mSize : nullptr; }
    const T* begin() const { return mData; }
    const T* end() const { return mData ? mData + mSize : nullptr; }
    T& operator[](size_t i) { return mData[i]; }
    const T& operator[](size_t i) const { return mData[i]; }
private:
    void Reset() { if (mData) mAllocator.release(mAllocator.context, mData); mData = nullptr; mSize = 0; }
    T* mData = nullptr;
    size_t mSize = 0;
    FontAllocator mAllocator;
};
struct GameGlyph {
    int16_t left=0, width=0, advance=0;
    uint16_t x=0, y=0, sheet=0;
};
struct GameFont {
    unsigned cellHeight=0, nominalHeight=0, ascent=0, sheetWidth=0, sheetHeight=0;
    uint16_t alternate=0;
    FontBuffer<GameGlyph> glyphs;
    FontBuffer<int32_t> codepoints;
    FontBuffer<uint8_t> alpha;
    FontBuffer<uint8_t> luminance; // empty for intensity-as-alpha fonts
    const GameGlyph* find(uint32_t codepoint, bool useAlternate=true) const;
    uint8_t sample(const GameGlyph&, unsigned x, unsigned y) const;
    uint8_t shade(const GameGlyph&, unsigned x, unsigned y) const;
    bool valid() const;
};
struct FontSet { const GameFont* menu=nullptr; const GameFont* message=nullptr; };
struct FontBundle {
    GameFont menu, message;
    FontSet view() const {return {menu.valid()?&menu:nullptr,message.valid()?&message:nullptr};}
};
bool ParseGameFont(const void*,size_t,GameFont&,const char*& error,const FontAllocator& = {});
bool ParseGameFontArchive(const void*,size_t,FontBundle&,const char*& error,const FontAllocator& = {});
// Always consumes at least one byte; invalid UTF-8 maps to U+FFFD.
uint32_t NextFontCodepoint(const std::string&,size_t& offset);
}
