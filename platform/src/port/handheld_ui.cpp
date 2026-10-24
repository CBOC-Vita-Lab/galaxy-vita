// Handheld consoles (PS Vita): the game's prompts name the buttons the player actually has.
//
// Two parts, both applied to what the game loads, never to the files:
//  - pictures: the game writes "press (A)" with a picture font whose glyphs are the Wii
//    controller's buttons. When the renderer decodes one of that font's texture sheets, the
//    cells of those glyphs are replaced by the handheld's buttons (tools/handheld/make_glyphs.py
//    draws them): A -> Cross, B -> Circle, C -> Triangle, Z -> L, + -> START, - -> SELECT, the
//    remote and the Nunchuk -> the console itself, and the unused "1" button -> Square.
//    The layouts' own A-button pictures become Cross, and the three animations of a hand with
//    the remote (shake to spin, lean for the rolling ball, twist for the ray) become a drawing
//    of the handheld with Square lit, or rolling and pitching (handheld_art.inc).
//  - words: "shake (remote)" becomes "press (Square)" and "point (remote) at the screen"
//    becomes "touch the screen", and the title screen's "press both (A) and (B)" becomes "Press START or
//    touch the screen" (English texts; the other languages keep their wording and get the pictures). The game's text is patched when it is looked up, into a copy.
// The controls these describe are in vita/runtime/input.cpp.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "port/heap_routing.h"
#include "port/port.h"

#include "handheld_art.inc"

extern "C" int port_handheld(void);

namespace {
#include "handheld_glyphs.inc"

// ------------------------------------------------------------------ pictures
// The picture font of the game's Font.arc (all regions' discs seen so far share it): six
// 128 x 128 sheets of 3 x 3 cells, recognised by the hash of their texels.
struct Cell { int glyph; const unsigned char* picture; };
struct Sheet { uint64_t hash; int first; }; // first: glyph index of the sheet's first cell
const Sheet kSheets[] = {{0x72c1d5ce90c4d26aull, 0}, {0x7962f85f36288d03ull, 18}};
const Cell kCells[] = {
    {1, kGlyphCross},    // A
    {2, kGlyphCircle},   // B
    {3, kGlyphTriangle}, // C
    {4, kGlyphConsole},  // the remote
    {5, kGlyphConsole},  // the Nunchuk
    {6, kGlyphSquare},   // "1" (no text uses it: the word rules below put it where the remote was shaken)
    {7, kGlyphR},        // "2" (unused)
    {24, kGlyphSelect},  // -
    {25, kGlyphStart},   // +
    {26, kGlyphL},       // Z
};

// Button pictures that are textures of their own in the game's layouts (the "next page" button of
// the storybook and of speech boxes), replaced whole.
struct Texture { uint64_t hash; uint32_t size; const unsigned char* picture; };
const Texture kTextures[] = {
    {0x2af20feb28c852fcull, 48, kTexCross48}, // A, 48 x 48
    {0xf2119da2a3ed2f3bull, 40, kTexCross40}, // A, 40 x 40
};

bool Enabled() {
    static const bool on = [] {
        const char* e = getenv("PETARI_HANDHELD_UI");
        return (e && *e) ? atoi(e) != 0 : port_handheld() != 0;
    }();
    return on;
}

struct TexturePatch {
    const Texture* texture = nullptr;
    const handheld_art::Entry* animation = nullptr;
    const Sheet* sheet = nullptr;
    explicit operator bool() const { return texture || animation || sheet; }
};
TexturePatch FindTexturePatch(uint64_t hash, uint32_t width, uint32_t height) {
    if (!Enabled()) return {};
    for (const Texture& t : kTextures)
        if (t.hash == hash && t.size == width && t.size == height) return {&t};
    if (width != 128 || height != 128) return {};
    if (const auto* animation = handheld_art::Find(hash)) return {nullptr, animation};
    for (const Sheet& s : kSheets)
        if (s.hash == hash) return {nullptr, nullptr, &s};
    return {};
}

// ------------------------------------------------------------------ words
// A message is UTF-16 with tags: 0x1A, then a byte with the tag's whole size in bytes (the 0x1A
// included), a group byte, a 16-bit tag number and parameters. Group 3 is a picture.
constexpr int kPicA = 0, kPicB = 1, kPicRemote = 3, kPicSquare = 5;
struct Tok {
    enum Kind : uint8_t { Word, Sep, Pic, Tag } kind;
    std::u16string text; // Word / Sep (one character) / Tag (the raw units, 0x1A first)
    int pic = 0;
};

std::vector<Tok> Tokenize(const uint16_t* s) {
    std::vector<Tok> out;
    std::u16string word;
    auto flush = [&] {
        if (!word.empty()) { out.push_back(Tok{Tok::Word, word, 0}); word.clear(); }
    };
    while (*s) {
        const uint16_t c = *s;
        if (c == 0x1A) {
            flush();
            const unsigned size = s[1] & 0xFF, group = (s[1] >> 8) & 0xFF;
            const unsigned units = size >= 4 ? size / 2 : 2;
            if (group == 3 && units >= 3) out.push_back(Tok{Tok::Pic, std::u16string(reinterpret_cast<const char16_t*>(s), units), s[2]});
            else out.push_back(Tok{Tok::Tag, std::u16string(reinterpret_cast<const char16_t*>(s), units), 0});
            s += units;
            continue;
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '\'') word.push_back(static_cast<char16_t>(c));
        else { flush(); out.push_back(Tok{Tok::Sep, std::u16string(1, static_cast<char16_t>(c)), 0}); }
        ++s;
    }
    flush();
    return out;
}
// The next word or picture after i: spaces, line breaks and other tags are skipped, punctuation ends the search.
int Next(const std::vector<Tok>& t, int i) {
    if (i < 0) return -1;
    for (size_t j = static_cast<size_t>(i) + 1; j < t.size(); ++j) {
        if (t[j].kind == Tok::Word || t[j].kind == Tok::Pic) return static_cast<int>(j);
        if (t[j].kind == Tok::Sep && t[j].text[0] != ' ' && t[j].text[0] != '\n') return -1;
    }
    return -1;
}
bool Is(const std::vector<Tok>& t, int i, const char* word) {
    if (i < 0 || t[static_cast<size_t>(i)].kind != Tok::Word) return false;
    const std::u16string& w = t[static_cast<size_t>(i)].text;
    size_t n = strlen(word);
    if (w.size() != n) return false;
    for (size_t k = 0; k < n; ++k) {
        char16_t c = w[k];
        if (c >= 'A' && c <= 'Z') c = static_cast<char16_t>(c + 32);
        if (c != static_cast<char16_t>(word[k])) return false;
    }
    return true;
}
bool IsPic(const std::vector<Tok>& t, int i, int pic) { return i >= 0 && t[static_cast<size_t>(i)].kind == Tok::Pic && t[static_cast<size_t>(i)].pic == pic; }
void SetWord(std::vector<Tok>& t, int i, const char* word, bool keepCase) {
    Tok& k = t[static_cast<size_t>(i)];
    const bool upper = keepCase && k.kind == Tok::Word && !k.text.empty() && k.text[0] >= 'A' && k.text[0] <= 'Z';
    k.kind = Tok::Word;
    k.text.clear();
    for (const char* p = word; *p; ++p) k.text.push_back(static_cast<char16_t>(*p));
    if (upper && k.text[0] >= 'a' && k.text[0] <= 'z') k.text[0] = static_cast<char16_t>(k.text[0] - 32);
}
void SetPic(std::vector<Tok>& t, int i, int pic) {
    Tok& k = t[static_cast<size_t>(i)];
    k.pic = pic;
    k.text[2] = static_cast<char16_t>(pic);
}
// Removes word / picture i and one plain space next to it.
void Drop(std::vector<Tok>& t, int i) {
    t.erase(t.begin() + i);
    const size_t u = static_cast<size_t>(i);
    if (u < t.size() && t[u].kind == Tok::Sep && t[u].text[0] == ' ') t.erase(t.begin() + i);
    else if (i > 0 && t[u - 1].kind == Tok::Sep && t[u - 1].text[0] == ' ') t.erase(t.begin() + i - 1);
}

bool ApplyRules(std::vector<Tok>& t) {
    bool changed = false;
    for (int i = 0; i < static_cast<int>(t.size()); ++i) {
        if (t[static_cast<size_t>(i)].kind != Tok::Word) continue;
        const bool shake = Is(t, i, "shake"), shaking = Is(t, i, "shaking");
        if (shake || shaking) {
            // "shake [your] (remote)" -> "press (Square)"
            const int j = Next(t, i);
            const int k = (Is(t, j, "your") || Is(t, j, "the")) ? Next(t, j) : j;
            if (!IsPic(t, k, kPicRemote)) continue;
            SetWord(t, i, shake ? "press" : "pressing", true);
            SetPic(t, k, kPicSquare);
            if (k != j) Drop(t, j);
            changed = true;
        } else if (Is(t, i, "press")) {
            // the title screen: "press both (A) and (B)" -> "press START or touch the screen" (START or a touch answers it: port_title_confirm)
            const int b0 = Next(t, i);
            if (!Is(t, b0, "both")) continue;
            const int pa = Next(t, b0), an = Next(t, pa), pb = Next(t, an);
            if (!(IsPic(t, pa, kPicA) && Is(t, an, "and") && IsPic(t, pb, kPicB))) continue;
            t.erase(t.begin() + b0, t.begin() + pb + 1);
            const char* words[] = {"START", "or", "touch", "the", "screen"};
            int at = b0;
            for (int w = 0; w < 5; ++w) {
                if (w) t.insert(t.begin() + at++, Tok{Tok::Sep, u" ", 0});
                Tok k{Tok::Word, u"", 0};
                for (const char* c = words[w]; *c; ++c) k.text.push_back(static_cast<char16_t>(*c));
                t.insert(t.begin() + at++, k);
            }
            changed = true;
        } else if (Is(t, i, "point") || Is(t, i, "aim")) {
            const int j = Next(t, i);
            if (Is(t, j, "at")) {
                // "point at the screen with (remote)" -> "touch the screen"
                const int a = Next(t, j), b = Next(t, a), c = Next(t, b), d = Next(t, c);
                if (!(Is(t, a, "the") && Is(t, b, "screen") && Is(t, c, "with") && IsPic(t, d, kPicRemote))) continue;
                SetWord(t, i, "touch", true);
                Drop(t, d);
                Drop(t, c);
                Drop(t, j);
                changed = true;
            } else {
                // "point|aim [your] (remote) [at the screen]" -> "touch the screen"
                const bool article = Is(t, j, "your") || Is(t, j, "the");
                const int k = article ? Next(t, j) : j;
                if (!IsPic(t, k, kPicRemote)) continue;
                const int a = Next(t, k), b = Next(t, a), c = Next(t, b);
                SetWord(t, i, "touch", true);
                if (Is(t, a, "at") && Is(t, b, "the") && Is(t, c, "screen")) {
                    Drop(t, a);
                    Drop(t, k);
                    if (article) Drop(t, j);
                } else if (article) {
                    SetWord(t, j, "the", false);
                    Tok& kk = t[static_cast<size_t>(k)];
                    kk.kind = Tok::Word;
                    kk.pic = 0;
                    kk.text = u"screen";
                } else {
                    Tok& kk = t[static_cast<size_t>(k)];
                    kk.kind = Tok::Word;
                    kk.pic = 0;
                    kk.text = u"screen";
                    t.insert(t.begin() + k, Tok{Tok::Sep, u" ", 0});
                    t.insert(t.begin() + k, Tok{Tok::Word, u"the", 0});
                }
                changed = true;
            }
        }
    }
    return changed;
}

struct Patched {
    uint64_t hash = 0;
    bool changed = false;
    std::vector<uint16_t> text;
};
std::mutex sLock;
std::unordered_map<const void*, Patched>* sPatched;

} // namespace

extern "C" {

// The renderer's hook (vgx: SetTexturePatchHook): a texture has been decoded to RGBA8, top row
// first; `hash` is that of its texels as the game holds them.
bool port_handheld_texture_needs_rgba(uint64_t hash, uint32_t width, uint32_t height) {
    return static_cast<bool>(FindTexturePatch(hash, width, height));
}
void port_handheld_texture(uint64_t hash, uint32_t width, uint32_t height, uint8_t* rgba) {
    const TexturePatch patch = FindTexturePatch(hash, width, height);
    if (patch.texture) {
        memcpy(rgba, patch.texture->picture, static_cast<size_t>(width) * height * 4);
        return;
    }
    // the three "how to hold the controller" animations: a drawing of the handheld instead
    if (patch.animation) {
        handheld_art::Draw(rgba, 128, patch.animation->pose);
        return;
    }
    if (patch.sheet) {
        for (const Cell& c : kCells) {
            const int k = c.glyph - patch.sheet->first;
            if (k < 0 || k >= 9) continue;
            const int x0 = (k % 3) * (kGlyphW + 1) + 1, y0 = (k / 3) * (kGlyphH + 1) + 1;
            for (int y = 0; y < kGlyphH; ++y) memcpy(rgba + (static_cast<size_t>(y0 + y) * 128 + x0) * 4, c.picture + static_cast<size_t>(y) * kGlyphW * 4, kGlyphW * 4);
        }
    }
}

// MessageData::getMessage: the text the game is about to show (16-bit units, tags inside).
// Returns it, or a copy with the handheld's wording. Game thread.
const void* port_handheld_message(const void* original) {
    if (!original || !Enabled()) return original;
    const uint16_t* s = static_cast<const uint16_t*>(original);
    uint64_t h = 1469598103934665603ull;
    size_t n = 0;
    bool candidate = false; // only texts with a picture of the remote can change
    for (; s[n]; ++n) {
        h = (h ^ s[n]) * 1099511628211ull;
        if (s[n] == 0x1A) {
            const unsigned units = (s[n + 1] & 0xFF) >= 4 ? (s[n + 1] & 0xFF) / 2 : 2;
            if (((s[n + 1] >> 8) & 0xFF) == 3 && units >= 3 && (s[n + 2] == kPicRemote || s[n + 2] == kPicA)) candidate = true;
            for (unsigned i = 1; i < units; ++i) h = (h ^ s[n + i]) * 1099511628211ull;
            n += units - 1;
        }
        if (n > 8192) return original;
    }
    if (!candidate) return original;
    PortHostAllocScope host; // (the copies live in the host's heap, not in a game heap)
    std::lock_guard<std::mutex> lock(sLock);
    if (!sPatched) sPatched = new std::unordered_map<const void*, Patched>();
    Patched& p = (*sPatched)[original];
    if (p.hash != h) {
        p.hash = h;
        std::vector<Tok> t = Tokenize(s);
        p.changed = ApplyRules(t);
        p.text.clear();
        if (p.changed) {
            for (const Tok& k : t)
                for (char16_t c : k.text) p.text.push_back(static_cast<uint16_t>(c));
            p.text.push_back(0);
            static const bool log = getenv("PETARI_HANDHELD_UI_LOG") != nullptr;
            if (log) {
                char line[512];
                size_t o = 0;
                for (size_t i = 0; i + 1 < p.text.size() && o + 8 < sizeof line; ++i) {
                    const uint16_t c = p.text[i];
                    if (c == 0x1A) {
                        const unsigned units = (p.text[i + 1] & 0xFF) >= 4 ? (p.text[i + 1] & 0xFF) / 2 : 2;
                        if (((p.text[i + 1] >> 8) & 0xFF) == 3) o += static_cast<size_t>(snprintf(line + o, sizeof line - o, "[pic%u]", p.text[i + 2]));
                        i += units - 1;
                    } else line[o++] = c == '\n' ? '/' : (c < 127 ? static_cast<char>(c) : '?');
                }
                line[o] = 0;
                port_log("handheld text: %s", line);
            }
        }
    }
    return p.changed ? static_cast<const void*>(p.text.data()) : original;
}

} // extern "C"
