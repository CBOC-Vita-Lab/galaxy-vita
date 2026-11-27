// Galaxy 3.0 multiplayer: the self puppet's hook for MarioActor's draw path (owner: puppet).
// When the local player's character is not the MarioActor's own (e.g. Toad, Luigi, Cosmic Mario on a Mario file), MpPuppets
// draws a puppet of the local player and the MarioActor's body, face and hands are not drawn (shadow, effects, sensors and
// everything else stay). The flag is a plain global bool: true only during a session while the self puppet is alive in the
// current scene; false otherwise, so single-player draws exactly as before.
#pragma once

extern bool gMpHideLocalBody;   // MpPuppets.cpp

namespace MpSelfPuppet {
    inline bool hideLocalBody() {
        return gMpHideLocalBody;
    }
}  // namespace MpSelfPuppet
