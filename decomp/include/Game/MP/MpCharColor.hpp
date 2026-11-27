// Galaxy 3.0 multiplayer: one colour per character (touches). Pure data, no game headers: the name tags, the Star Pointers, the
// party strip's pips, the lists' pips (results, standings, players), the chat balloons and the character picker's plates all read this
// one table, so a player is the same colour everywhere. Indexed by MpLook: Mario red, Luigi green, Toad blue, Cosmic Mario purple,
// Yellow Toad yellow, Cosmic Luigi teal. `fill` is bright enough for text and marks on a dark or a bright background (the game's own
// name plate and the panels draw a dark outline round it); `ink` is the same hue dark enough to read on the balloon's white.
#pragma once

#include <stdint.h>

namespace MpCharColor {

struct C { uint8_t r, g, b; };
constexpr int kCount = 6;
constexpr C kFill[kCount] = {{255, 80, 72}, {96, 224, 72}, {72, 140, 255}, {204, 108, 255}, {255, 226, 60}, {48, 224, 208}};
constexpr C kInk[kCount] = {{176, 24, 24}, {20, 120, 40}, {28, 72, 176}, {104, 48, 168}, {150, 108, 0}, {0, 112, 104}};

constexpr int clampLook(int look) { return look >= 0 && look < kCount ? look : 0; }
constexpr C fill(int look) { return kFill[clampLook(look)]; }
constexpr C ink(int look) { return kInk[clampLook(look)]; }
// the dark edge of a mark (the fill at 40 %)
constexpr C edge(int look) { return C{static_cast<uint8_t>(kFill[clampLook(look)].r * 2 / 5), static_cast<uint8_t>(kFill[clampLook(look)].g * 2 / 5),
                                      static_cast<uint8_t>(kFill[clampLook(look)].b * 2 / 5)}; }

}  // namespace MpCharColor
