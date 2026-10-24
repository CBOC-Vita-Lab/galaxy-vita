// Render-thread half of the renderer: turns recorded frames into GLES 3.2
// draws.  A frame is prepared once (uploads, programs, per-draw state) and
// can then be executed any number of times: per eye, per display refresh,
// or once for a flat view.
#pragma once

#include <GLES3/gl32.h>

#include <memory>

#include "gx_record.h"

namespace gpu {

// An "EFB": color + depth-stencil textures the game's draws render into.
struct EfbTarget {
    GLuint fbo = 0, color = 0, depth = 0;
    int width = 0, height = 0;
};

// The frame's main 3D camera as recorded by the game (CMD_CAMERA).
struct CameraInfo {
    bool valid = false;
    float view[12];   // world -> view, 3x4 rows
    float pos[3];     // camera position (world)
    float watch[3];   // point the camera looks at (usually the player)
    float up[3];      // camera up vector
    float watchUp[3]; // up (anti-gravity) at the watch point
    float fovy;       // degrees
    uint32_t flags;   // PORT_GX_CAMERA_* hints
    float aspect;     // width / height of the game's projection
    float player[3];  // player centre (world), with PORT_GX_CAMERA_PLAYER
    float playerUp[3];  // against the player's gravity (world); zero while it has none
};

// How draws between the HUD markers are handled.
enum class HudMode {
    Inline,  // drawn into the EFB like everything else
    Target,  // drawn into the separate HUD target (premultiplied alpha)
    Skip,    // not drawn (the HUD was rendered for another eye)
};

// VR camera for perspective 3D draws (row-major 4x4 matrices).
struct EyeView {
    float view[16];  // game view space -> eye space (head tracking, IPD, world scale)
    float proj[16];  // eye space -> GL clip space
    // Cutaway: 3D geometry inside a cone from the eye to `focus` (eye space,
    // metres; w > 0 enables it) and inside a sphere around the eye is
    // dithered out, so walls never hide the player.  cut = radius at the eye,
    // radius at the focus, distance before the focus where the cut ends,
    // radius of the sphere around the eye.
    float focus[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float cut[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // The eye's position in game view space: lights the game puts on its
    // camera (view space origin) are lit from here instead.
    float eyePos[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int index = 0;  // 0 = left, 1 = right: each eye keeps its own EFB copies
    // One picture of a stereo pair of the flat view instead (view, proj and
    // the cutaway are not used): the frame is replayed with the game's own
    // camera, viewports and projection, and the 3D draws a VR eye would take
    // are shifted sideways in clip space by
    //   stereo[0] * min(stereo[1] - w, stereo[2] * w)
    // with w the depth in front of the camera (game units).  stereo[0]: the
    // shift of the farthest things, in half screen widths, positive for the
    // left picture and negative for the right one; stereo[1]: the depth
    // that lands where it does without the shift (the screen's own depth to
    // the two eyes); stereo[2]: how many times farther than the farthest
    // things the nearest ones may shift the other way; stereo[3]: the shift
    // of the draws between the HUD markers, in half screen widths (positive
    // for the left picture: the HUD floats before the screen).
    // pointer[0]: the shift of the pointer's 2D cursor (the draws between
    // the pointer markers) while it points into the scene: that of the point
    // under it; pointer[1]: nonzero while it points at menus instead, and
    // goes with the HUD.
    bool flatStereo = false;
    float stereo[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float pointer[2] = {0.0f, 0.0f};
    // SpaceWarp: with a target here, draws between the player markers leave
    // 1 in the stencil buffer where they write depth (other draws 0), and at
    // the scene depth marker the depth and stencil are copied into the
    // target (a depth-stencil target of any size, see createDepthTarget).
    EfbTarget* depthSnapshot = nullptr;
};

class Renderer {
public:
    bool init();
    EfbTarget createTarget(int width, int height);
    // Depth-stencil only (no color), sampled with nearest filtering.
    EfbTarget createDepthTarget(int width, int height);
    void destroyTarget(EfbTarget& t);
    // Fixed foveated rendering for a target (GL_QCOM_texture_foveated):
    // resolution falls off away from the focal point (normalized device
    // coordinates) at the given gain.  Returns false if unsupported.
    bool setFoveation(EfbTarget& t, float focalX, float focalY, float gain, float foveaArea);

    // Makes `frame` the frame to render, preparing it on this thread if it
    // is new.
    void setFrame(std::shared_ptr<const Frame> frame);
    // Asks the preparation thread for the newest recorded frame and switches
    // to the most recently prepared one.  Returns true if the frame changed.
    bool update();
    bool hasFrame() const;
    uint64_t frameNumber() const;

    // Replays the current frame into `efb`.  With `eye`, the perspective 3D
    // draws after the frame's camera use the VR camera (or, with
    // eye->flatStereo, the game's camera shifted for a stereo pair).
    void render(EfbTarget& efb, const EyeView* eye, EfbTarget* hud, HudMode hudMode = HudMode::Inline);
    // Whether the last render() reached the scene depth marker and filled
    // eye->depthSnapshot.
    bool snapshotTaken() const;
    // Draws the last render() sent to the HUD target (0: it stayed clear).
    int hudDrawCount() const;

    // Camera recorded in the current frame (valid = false if none).
    const CameraInfo& camera() const;

    // Texture holding the last EFB->XFB copy of the most recent render().
    GLuint xfbTexture() const { return mXfbTex; }
    int xfbWidth() const { return mXfbWidth; }
    int xfbHeight() const { return mXfbHeight; }

    struct Impl;

private:
    GLuint mXfbTex = 0;
    int mXfbWidth = 0, mXfbHeight = 0;
};

Renderer& renderer();

// File for linked shader program binaries (set before Renderer::init).
void setShaderCachePath(const char* path);

// Debug: stop replaying after this many draws (-1 = all).  Initialized from
// PETARI_GLSTOP.
extern int gDebugStopAfterDraws;

}  // namespace gpu
