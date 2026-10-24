#include "Game/Util/ShareUtil.hpp"

ResourceShare::~ResourceShare() {
}

ResourceShare::ResourceShare() : NameObj("\x8e\x91\x8c\xb9\x8b\xa4\x97\x4c\x8b\x40\x8d\x5c") {
    _C = new u8[0x80];
    _10 = new u8[0x80];
    _14 = 0;
}
