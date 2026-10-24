// Interface between the OpenXR frame loop and the game presentation
// (vr_game.cpp).
#pragma once

#include <GLES3/gl32.h>

#include <string>
#include <vector>

#include "xmath.h"

namespace vr {

struct EyeInfo {
    xm::Mat4 proj;       // eye projection (GL clip space)
    xm::Mat4 view;       // eye-from-stage
    xm::Vec3 position;   // eye position in stage space (metres)
    xm::Quat orientation;
    int width, height;   // swapchain image size (swapchainScale())
};

struct FrameInfo {
    EyeInfo eyes[2];
    double time;  // predicted display time, seconds
    float cullTanX, cullTanY;  // widest half-FOV tangents over both eyes
    // GPU time one eye may take, ms: the display refresh when a game frame
    // has two refreshes for its two eyes, half of it when both eyes share
    // one.  0 keeps the render scale fixed.
    float eyeBudgetMs;
    // SpaceWarp: the frame's motion vectors will be rendered (renderMotion),
    // so the eyes keep the scene's depth.
    bool motion;
};

// Near and far planes of the eye projections, metres (the depth images
// given to the compositor for SpaceWarp use them too).
const float kNearZ = 0.05f, kFarZ = 1000.0f;

void init();

// Optional comfort settings (petari_vr.ini: "key = value" lines, # comments):
//   diorama_scale     size of the world, 1 = default (1/500 of the game's)
//   diorama_height    Mario's height below the eyes, metres (default 0.8)
//   diorama_distance  Mario's distance in front, metres (default 1.5)
//   follow_smoothing  half-life of the world following Mario, s (0.12)
//   turn_smoothing    half-life of the world turning with the camera, s (0.35)
//   vignette          strength of the vignette while turning, 0..1 (1)
//   cutaway           1 to see Mario through walls, 0 to turn it off (1)
//   skip_hold         seconds A is held to skip a cutscene, 0 = never (1)
//   resolution        largest eye size relative to the headset's recommended
//                     size (1.6); the render scale moves between
//                     min_resolution and this with the GPU load (dynamic
//                     resolution)
//   min_resolution    the lowest render scale (0.8; up to 1.25): higher
//                     stays sharper, but frames come late when the GPU
//                     cannot keep up
//   refresh_rate      display refresh rate, Hz (120: each game frame stays
//                     up for exactly two refreshes)
//   high_clocks       1 to ask for Meta's SustainedHigh CPU/GPU levels (1),
//                     0 to leave the clocks to the system (saves battery)
//   space_warp        1 to show the game's 60 frames a second at 120 Hz with
//                     Meta's Application SpaceWarp (1): the compositor
//                     synthesizes every other frame from motion vectors
//                     instead of showing each frame twice; 0 turns it off
//   super_resolution  1 (default) to hand the diorama to the compositor at
//                     its render size and have Meta Quest Super Resolution
//                     scale and sharpen it; 0 scales it in the app (bilinear)
//   giant_screen      1 (default) to play on a giant virtual screen, from
//                     the game's own camera; 0 for the diorama
//   screen_distance   how far away the giant screen is, metres (4.5; it is
//                     5.33 m wide, so 61 deg across at 4.5 m, 30 at 10 m)
//   stereo_screen     1 to show the giant screen's picture in stereoscopic
//                     3D, a picture for each eye from the game's camera
//                     moved a little to either side (0, the default: the
//                     same picture for both eyes)
//   stereo_depth      how deep that 3D is, 0.25 .. 3 (1.75; "3D depth" on
//                     the settings panel): how far before the farthest
//                     things Mario is drawn, in distances between your eyes.
//                     About 0.5 keeps the whole scene behind the screen
//                     (little depth); more brings Mario and the ground
//                     before him out of the screen
//   stereo_far        how far apart the farthest things are drawn, in
//                     distances between your eyes, 0.5 .. 1 (0.95; 1 puts
//                     them at infinity)
//   stereo_resolution size of each of the two pictures relative to the
//                     screen's single one (2048x1152), 0.5 .. 1 (0.8): two
//                     pictures cost the GPU twice one, and at 0.8 they
//                     still have about a texel for each display pixel with
//                     the screen 4.5 m away
//   game_path         the folder of the game's files chosen on the setup
//                     screen (empty: the app's own folder, files/game)
//   turn_with_camera  1 to have the diorama turn with the game camera as it
//                     swings round Mario (0: the world keeps its facing; the
//                     right stick turns it in steps)
//   sharpening        1 to sharpen (and scale) the diorama with AMD
//                     FidelityFX CAS as it is composited; 0 (default) off
//   sharpening_strength  CAS strength, 0 .. 1 (default 0.5)
//   invert_camera     1 (default) swaps the right stick's left and right
//                     where they turn the camera: pushing it right turns the
//                     view to the right (the camera goes round Mario to the
//                     left); 0 moves the camera the way the stick is pushed,
//                     as the Wii's D-pad did
//   passthrough       1 to see the room around the giant screen (the
//                     headset's passthrough) instead of the dark; 0 (default)
//   language          the game's language by name (english, french, german,
//                     spanish, italian; japanese, korean), among those the
//                     disc has: read when the game starts
// Call before init() and before creating the swapchains; a missing file
// keeps the defaults.
void loadSettings(const char* path);
// A numeric setting of that file: its key, the values it may take, its
// default and how to read and change it.  Changes apply at once, except
// high_clocks (asked for when the headset session begins).  The settings
// panel works through these.
struct Setting {
    const char* key;
    float min, max, def;
    float (*get)();
    void (*set)(float);
};
const Setting* findSetting(const char* key);
// Swapchain image size relative to the recommended eye size: the
// resolution setting when they are made, at most 1.25 (the game renders at
// the dynamic scale and the composite resamples).
float swapchainScale();
// The refresh rate asked for (refresh_rate; the frame loop follows changes),
// and the rates the display has (from the frame loop, for the panel).
float refreshRate();
void setRefreshRates(const float* rates, int count);
int refreshRates(float* rates, int max);
bool highClocks();
// The right stick's camera turns swapped (invert_camera).
bool invertCamera();
// The room around the giant screen (the passthrough setting).  Wanted: the
// setting is on and the giant screen with it, so the headset's passthrough
// should run.  Shown: how far the eye images are see-through around the
// screen just now, 0 .. 1 (it fades): while above 0, the frame loop puts the
// passthrough layer under the eye layer and blends that one by its alpha.
// Available: the headset has passthrough and it started (the frame loop
// says); without it the dark stays.
bool passthroughWanted();
float passthroughShown();
bool passthroughAvailable();
void setPassthroughAvailable(bool available);
// SpaceWarp on (the setting, or the settings panel's switch since).
bool spaceWarp();
// Meta Quest Super Resolution on the eye images (super_resolution setting).
bool superResolution();
// A snap turn of the diorama: its view goes a step (45 deg) round Mario to
// the right (dir > 0) or the left, behind a blink.  Returns false (the
// press is the game's D-pad then) unless the diorama is shown with its own
// yaw (not turn_with_camera).
bool snapTurn(int dir);
// The render scale now (the dynamic resolution moves it between the
// min_resolution and resolution settings), and the scale of the screen's 3D
// pictures (the giant screen's gameplay): 1 is the full picture, and the
// GPU's load moves it down to three quarters of min_resolution.
float renderScale();
float screenPictureScale();

// Compositor layers for the panels that carry text (xr_app.cpp submits them
// over the eye layer, as Meta recommends for text and UI): the compositor
// samples them once, at full sharpness whatever size the eye images are
// rendered at, Super Resolution or not.  While they are on, the eye images
// leave those panels out.
enum { kBothEyes, kLeftEye, kRightEye };
struct UiLayer {
    bool visible = false;               // goes out this frame
    bool changed = false;               // its image needs drawing (drawUiLayer)
    xm::Vec3 position{0, 0, 0};         // centre, stage space
    xm::Quat orientation{0, 0, 0, 1};   // the image faces +Z of this
    float width = 0.0f, height = 0.0f;  // metres
    int eye = kBothEyes;                // which eyes see it
    // The part of the layer's image that holds the picture, from its lower
    // left corner (0: all of it).
    int imageWidth = 0, imageHeight = 0;
};
// kScreenRightLayer: the right eye's picture of the virtual screen while it
// shows a stereo pair (kScreenLayer is the left eye's then, and both eyes'
// otherwise).
enum { kHudLayer, kSettingsLayer, kScreenLayer, kSetupLayer, kScreenRightLayer, kUiLayerCount };
// The UI layers from the bottom one to the top one.
const int kUiLayerOrder[kUiLayerCount] = {kScreenLayer, kScreenRightLayer, kHudLayer, kSettingsLayer, kSetupLayer};
void setUiLayers(bool on);
bool uiLayers();
// Image size of a UI layer (its swapchain).
void uiLayerSize(int which, int* width, int* height);
// A UI layer's state after the eyes of a frame or refresh were rendered.
UiLayer uiLayer(int which);
// Draws UI layer `which` into `fbo` (a swapchain image of uiLayerSize;
// premultiplied alpha, sRGB).
void drawUiLayer(int which, GLuint fbo);
// Composites a UI layer's image (`texture`, as drawUiLayer left it) into
// the bound eye image, as the compositor would (the headless simulator).
void compositeUiLayer(int which, GLuint texture, const xm::Mat4& viewProj);

// The runtime's performance counters (XR_META_performance_metrics), after
// each frame: the GPU time of the app's last frame and of the compositor's
// last refresh (ms, < 0 when unknown), the GPU's utilization (%, < 0 when
// unknown), and how many eyes and display refreshes an app frame spans.
// With them the dynamic resolution judges the eyes by the runtime's measure
// (the timer queries miss the tiler's resolves) and takes the compositor's
// share of each refresh into account.
void notePerformance(float appGpuMs, float compositorGpuMs, float gpuUtilization, int eyesPerFrame, int refreshesPerFrame, float refreshMs);
void beginFrame(const FrameInfo& frame);
// Display refreshes the frame loop just missed (the dynamic resolution
// steps down when they add up).
void noteMissedRefreshes(int count);
// Renders eye `eye` into `fbo` (a swapchain image, sRGB, width x height).
// Returns the part of the image used, from its lower left corner: with
// Super Resolution, the diorama at its render size when that fits (the
// compositor scales it to the display in one step, with Meta Quest Super
// Resolution); otherwise the whole image.
struct Extent {
    int width, height;
};
Extent renderEye(int eye, const FrameInfo& frame, GLuint fbo, int width, int height);

// SpaceWarp (Meta's XR_FB_space_warp).  setMotionSize: the size of the
// motion vector and depth images, before the first frame with
// FrameInfo::motion.  renderMotion: after renderEye for both eyes of such a
// frame, eye `eye`'s motion vectors (into `motionTex`, RGBA16F) and depth
// (`depthTex`, GL_DEPTH24_STENCIL8).  finishMotion: once both eyes are
// done, the frame's app-space delta pose (the room's pose in the game world
// relative to the last frame's, as XrCompositionLayerSpaceWarpInfoFB wants
// it); returns true when the compositor should not extrapolate from the
// frame (the view changed as a whole: a cut, a fade between the diorama and
// the virtual screen).
void setMotionSize(int width, int height);
void renderMotion(int eye, const FrameInfo& frame, GLuint motionTex, GLuint depthTex);
bool finishMotion(xm::Quat* deltaOrientation, xm::Vec3* deltaPosition);
// Debug view of a motion vector image (motion size, RGBA8, rows bottom up
// as port_headless_write_png takes them):
// red / green = horizontal / vertical motion around grey (1/40 of the view
// per frame saturates), blue = 0 world, 128 fixed in the room, 255 player;
// `summary` gets the average motion of each kind of pixel.
void motionDebugImage(GLuint motionTex, std::vector<unsigned char>* rgba, std::string* summary);
// Maps a controller aim ray (stage space) to the Wii pointer (-1..1).
bool pointerFromRay(xm::Vec3 origin, xm::Vec3 dir, float* x, float* y);
// The controller's aim is not tracked: no laser until pointerFromRay again.
void pointerLost();

// True once for each new target the game's pointer touched since the last
// call (for a haptic tick).
bool takePointerTouch();
// The ray is on one of the VR layer's own panels (after pointerFromRay for
// the frame): the length of the laser drawn from the controller, metres.
void setAimLength(float metres);

// Draws a premultiplied overlay texture with the model-view-projection
// `mvp` (a unit quad around the origin), faded by `alpha`.
void drawOverlayQuad(GLuint texture, const xm::Mat4& mvp, float alpha);

// The settings panel beside the game's pause menu (vr_settings.cpp).
// settingsInit: after the GL context exists; changes are saved to `iniPath`.
void settingsInit(const char* iniPath);
// Each input update, with the pointer's aim ray (stage space) and whether a
// click button (A or the trigger) is held: returns the distance along the
// ray to the panel while the ray is on it (the game then gets no pointer),
// 0 otherwise.
float settingsPointer(xm::Vec3 origin, xm::Vec3 dir, bool clickDown);
// True while the panel keeps the click buttons from the game (a click made
// on it).
bool settingsOwnsClick();
// True once each time the pointer moves onto one of the panel's controls.
bool settingsTakeTick();
// True while the panel is on screen (fading in or out included).
bool settingsShown();
// The panel as a UI layer (vr_game.cpp): its size in pixels, its state, and
// its picture (with the pointer's reticle) drawn to fill the viewport.
void settingsLayerSize(int* width, int* height);
UiLayer settingsLayer();
void settingsDrawLayer();
// Draws a round reticle centred at (x, y) with radii rx, ry in the bound
// viewport's clip space (the settings panel's layer image).
void drawReticle2d(float x, float y, float rx, float ry);
void settingsDraw(const xm::Mat4& viewProj);

// The game's files: the folder chosen on the setup screen (game_path in
// petari_vr.ini; empty until one is), and saving a new choice.
const std::string& gamePath();
void saveGamePath(const std::string& path);
// Whether `dir` holds the game's files (sys/fst.bin and files/), and if so
// whether they are converted for the port (tools/cook/cook.py); *outdated:
// converted by an older converter, without the data it now also takes from
// main.dol (sys/ErrorMessageArchive.arc and two tables); *unknown: converted,
// but the files of no disc the port knows (port_dvd_identify).
bool isGameFolder(const std::string& dir, bool* ready, bool* outdated = nullptr, bool* unknown = nullptr);

// The setup screen (vr_setup.cpp), shown instead of the game while its files
// are missing.  setupStart opens it (after vr::init) and searches the app's
// own folder `appDir` (and, with all files access, the headset's storage);
// `tried` is the folder the app looked in.
void setupStart(const std::string& appDir, const std::string& tried);
bool setupActive();
// Whether the app may read all files (Android's all files access); a change
// starts a new search.
void setupSetStorageAccess(bool granted);
// Pointer input, as settingsPointer: the distance to the panel along the
// ray while it is on it, 0 otherwise.
float setupPointer(xm::Vec3 origin, xm::Vec3 dir, bool clickDown);
bool setupTakeTick();
// The folder the player chose (once): the screen closes, the caller boots
// the game from it.
bool setupTakeChoice(std::string* folder);
// True once after the player asked for all files access on the screen: the
// caller opens Android's settings page for it.
bool setupTakeAccessRequest();
// The screen as a UI layer, and drawn into the eye images without them.
void setupLayerSize(int* width, int* height);
UiLayer setupLayer();
void setupDrawLayer();
void setupDraw(const xm::Mat4& viewProj);

}  // namespace vr
