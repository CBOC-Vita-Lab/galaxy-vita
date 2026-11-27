// Galaxy 3.0 multiplayer: first-appearance warm-up (owner: puppetprep). Everything a multiplayer session draws for the first time in
// play (a remote puppet, a multiplayer bubble, a name tag, a pointer mark) is drawn once, out of sight, in the scene's first
// pictures, while the opening wipe still covers them, so its models, textures and shader variants are ready (a texture upload or a program
// build on the first visible picture is a hitch, on the Vita a shader compile). Nothing is allocated: the puppets, the bubble pool,
// the plates and the marks are made by their own modules during the scene load; this only draws them once. No-op without a session.
#pragma once

namespace MpWarm {
    void sceneInit();     // MpGame::onGameSceneInitAfterPlacement, after every module made its scene objects
    void sceneEnd();      // scene teardown
    void onDraw();        // MpPuppetDrawer::draw, once per picture: the warm-up's pictures
}
