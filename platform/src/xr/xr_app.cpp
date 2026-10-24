// OpenXR application: EGL/GLES context, session lifecycle, swapchains, Touch
// controller actions, and the headset frame loop.
//
// Quest requirements verified against Meta's docs (2026-09-28): Khronos loader
// initialised through xrInitializeLoaderKHR + XrLoaderInitInfoAndroidKHR,
// XR_KHR_android_create_instance, Touch Plus via XR_META_touch_controller_plus.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "port/heap_routing.h"
#include "port/input.h"
#include "port/input_script.h"
#include "port/port.h"
#include "../gx/gl_renderer.h"
#include "vr_renderer.h"
#include "xmath.h"

extern "C" void port_boot(const char* dataRoot, const char* saveRoot);
extern "C" void port_mem_set_reserved_window(uintptr_t base, size_t size);
extern "C" void port_headless_write_png(const char* path, const unsigned char* rgba, int w, int h);
extern "C" int port_vr_diorama(void);  // the diorama is on show (vr_game.cpp)

namespace {

#define XR_CHECK(call)                                                                                                                               \
    do {                                                                                                                                             \
        XrResult _r = (call);                                                                                                                        \
        if (XR_FAILED(_r)) {                                                                                                                         \
            port_fatal("%s failed: %d (%s:%d)", #call, (int)_r, __FILE__, __LINE__);                                                                 \
        }                                                                                                                                            \
    } while (0)

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t width = 0, height = 0;
    uint32_t mips = 1;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    std::vector<GLuint> fbos;
};

struct App {
    android_app* android = nullptr;

    // EGL
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    // OpenXR
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    // The app's space: the runtime's LOCAL space turned and moved so the
    // player faces the screen where they stand when a session starts (see
    // updateAppSpace).
    XrSpace appSpace = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE;
    bool facePlayerDue = false;  // a session began, or the player recentred the view: the app's space is set to the head's pose
    XrTime facePlayerAfter = 0;  // not before this time (when a recentring takes effect)
    // A session's first seconds: the head's pose in LOCAL is watched for a
    // jump (the headset finding its place in the room after waking up).
    bool faceWatchStart = false;
    XrTime faceWatchUntil = 0;
    bool lastHeadValid = false;
    float lastHeadYaw = 0.0f;
    XrVector3f lastHeadPos{};
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;
    bool focused = false;
    bool pausedInGameplay = false;  // focus was lost during ordinary gameplay
    uint32_t dpad = 0;              // D-pad direction held by the right stick
    bool dpadTurned = false;        // that flick was a snap turn of the diorama, not the D-pad
    bool aimTracked = true;         // the right controller's aim pose is known
    int lastRumble = 0;
    bool hasRefreshRate = false;
    bool hasTouchPlus = false;
    bool hasPerfSettings = false;
    bool hasSpaceWarp = false;  // XR_FB_space_warp enabled, with its swapchains
    bool hasLayerSettings = false;  // XR_FB_composition_layer_settings (Super Resolution)
    bool hasRecommendedRes = false; // XR_META_recommended_layer_resolution (Meta's dynamic resolution)
    PFN_xrGetRecommendedLayerResolutionMETA xrGetRecommendedLayerResolutionMETA = nullptr;
    // Meta's performance counters (XR_META_performance_metrics): the app's
    // and the compositor's GPU time and the GPU's utilization, for the
    // dynamic resolution and the log.
    bool hasPerfMetrics = false;
    PFN_xrEnumeratePerformanceMetricsCounterPathsMETA xrEnumeratePerformanceMetricsCounterPathsMETA = nullptr;
    PFN_xrSetPerformanceMetricsStateMETA xrSetPerformanceMetricsStateMETA = nullptr;
    PFN_xrQueryPerformanceMetricsCounterMETA xrQueryPerformanceMetricsCounterMETA = nullptr;
    XrPath appGpuPath = XR_NULL_PATH, compositorGpuPath = XR_NULL_PATH, gpuUtilPath = XR_NULL_PATH;
    // The panels with text as quad layers over the eye layer (vr::uiLayer).
    Swapchain ui[vr::kUiLayerCount];
    bool uiHasImage[vr::kUiLayerCount] = {};
    int64_t recLogAt = 0;           // last log of the runtime's recommendation
    int recValid = 0, recSamples = 0;
    int recMinW = 0, recMaxW = 0;
    // SpaceWarp's motion vector and depth images, per eye, at the size the
    // runtime recommends.
    Swapchain motion[2], depth[2];
    uint32_t motionW = 0, motionH = 0;
    float displayHz = 0.0f;  // the display's refresh rate, once known
    // Two sets of eye swapchains, [set][eye]: consecutive game frames are
    // rendered into alternate sets, so one set can be on display while the
    // other is being rendered (see renderFrame).
    Swapchain eyes[2][2];
    XrViewConfigurationView viewConfig[2]{};

    // Frame pacing (renderFrame).
    int renderSet = 0;       // set the next game frame is rendered into
    int shownSet = -1;       // set submitted
    int renderedSet = -1;    // paired refreshes: set finished, submitted from the next refresh on
    bool renderDue = false;  // paired refreshes: the right eye of the game frame picked up is due
    int heldSet = -1;        // paired refreshes: set whose left eye image is acquired and rendered, not yet released
    uint32_t heldIdx = 0;
    XrTime setTime = 0;      // display time the game frame picked up is rendered for
    XrPosef setPose[2][2];  // [set][eye] pose and field of view each image was rendered with
    XrFovf setFov[2][2];
    XrExtent2Di setRect[2][2];  // [set][eye] part of each image rendered (vr::renderEye)
    vr::FrameInfo setFrame{};  // the game frame picked up (from its update refresh)
    bool paired = false;       // the display refreshes at 120 Hz: game frames take two refreshes each
    XrTime lastDisplayTime = 0;
    int skippedRefreshes = 0;  // refreshes the frame loop missed (logged)
    int lateRefreshes = 0;     // of those, missed while the frame loop's own work ran late
    int64_t skipLogAt = 0;
    int64_t workStartNs = 0, lastWorkNs = 0;  // the last refresh's work, from xrWaitFrame to xrEndFrame
    bool warp = false;          // SpaceWarp: each frame goes out with motion vectors
    int warpFrames = 0;         // frames since SpaceWarp started (the first few are logged)
    XrTime warpLastTime = 0;    // display time of the last SpaceWarp frame
    XrTime lastRetraceTime = 0; // SpaceWarp: display time of the frame that started the last game frame
    bool warpSkip = false;      // the frame just rendered is not to be extrapolated
    XrPosef warpDelta{};        // its app-space delta pose

    // Input
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction moveAction, lookAction, aAction, bAction, xAction, yAction, triggerAction, gripAction, menuAction, stickClickAction,
        aimPoseAction, hapticAction;
    XrPath handPath[2];
    XrSpace aimSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

    PFN_xrRequestDisplayRefreshRateFB xrRequestDisplayRefreshRateFB = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB xrEnumerateDisplayRefreshRatesFB = nullptr;
    PFN_xrGetDisplayRefreshRateFB xrGetDisplayRefreshRateFB = nullptr;
    PFN_xrPerfSettingsSetPerformanceLevelEXT xrPerfSettingsSetPerformanceLevelEXT = nullptr;
    float requestedHz = 0.0f;  // the refresh rate last asked for (the refresh_rate setting may change in play)

    // The room around the giant screen (the passthrough setting): Meta's
    // passthrough (XR_FB_passthrough) as a layer under the eye layer, made
    // when the setting first asks for it and paused while it does not.
    bool hasPassthrough = false;
    XrPassthroughFB passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthroughLayer = XR_NULL_HANDLE;
    bool passthroughRunning = false;
    PFN_xrCreatePassthroughFB xrCreatePassthroughFB = nullptr;
    PFN_xrPassthroughStartFB xrPassthroughStartFB = nullptr;
    PFN_xrPassthroughPauseFB xrPassthroughPauseFB = nullptr;
    PFN_xrCreatePassthroughLayerFB xrCreatePassthroughLayerFB = nullptr;
    PFN_xrPassthroughLayerResumeFB xrPassthroughLayerResumeFB = nullptr;
    PFN_xrPassthroughLayerPauseFB xrPassthroughLayerPauseFB = nullptr;
};

App gApp;

// Test hooks: petari_debug.env next to the game data ("NAME=value" lines)
// sets the PETARI_* debug variables the headless runner takes, before the
// game boots.  PETARI_INPUT then scripts the controllers, timed from boot,
// so the headset can be tested without wearing it.
PortInputEvent gScript[1024];
int gScriptCount = 0;
int64_t gBootNs = 0;
// The game runs once its files are found (port_boot); until then the setup
// screen is up (vr_setup.cpp) and the frame loop only draws it.
bool gBooted = false;
std::string gSaveRoot;
int64_t gAccessCheckAt = 0;

void loadDebugEnv(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        return;
    }
    static char line[16384];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char* eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        port_log("%s: %s=%.60s%s", path.c_str(), line, eq + 1, strlen(eq + 1) > 60 ? "..." : "");
    }
    fclose(f);
}

// ---------------------------------------------------------------------------
// EGL
// ---------------------------------------------------------------------------
void initEgl(App& a) {
    a.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(a.display, nullptr, nullptr);
    const EGLint cfgAttr[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
                              EGL_SAMPLES, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLint num = 0;
    if (!eglChooseConfig(a.display, cfgAttr, &a.config, 1, &num) || num == 0) {
        port_fatal("eglChooseConfig failed");
    }
    const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    a.context = eglCreateContext(a.display, a.config, EGL_NO_CONTEXT, ctxAttr);
    if (a.context == EGL_NO_CONTEXT) {
        port_fatal("eglCreateContext failed: 0x%x", eglGetError());
    }
    const EGLint pbAttr[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    a.surface = eglCreatePbufferSurface(a.display, a.config, pbAttr);
    eglMakeCurrent(a.display, a.surface, a.surface, a.context);
    port_log("GL: %s / %s / %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
}

// ---------------------------------------------------------------------------
// OpenXR instance / session
// ---------------------------------------------------------------------------
bool hasExtension(const std::vector<XrExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts) {
        if (strcmp(e.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

void initInstance(App& a) {
    PFN_xrInitializeLoaderKHR initLoader = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&initLoader));
    XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInfo.applicationVM = a.android->activity->vm;
    loaderInfo.applicationContext = a.android->activity->clazz;
    XR_CHECK(initLoader((const XrLoaderInitInfoBaseHeaderKHR*)&loaderInfo));

    uint32_t count = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
    std::vector<XrExtensionProperties> exts(count, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data()));

    std::vector<const char*> enable = {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME};
    a.hasRefreshRate = hasExtension(exts, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (a.hasRefreshRate) {
        enable.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }
    a.hasTouchPlus = hasExtension(exts, XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);
    if (a.hasTouchPlus) {
        enable.push_back(XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);
    }
    a.hasPerfSettings = hasExtension(exts, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if (a.hasPerfSettings) {
        enable.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    }
    // Application SpaceWarp: only with the refresh rate extension, which
    // tells when the display runs at 120 Hz.  Enabled whatever the
    // space_warp setting, which the settings panel can change in play.
    a.hasSpaceWarp = a.hasRefreshRate && hasExtension(exts, XR_FB_SPACE_WARP_EXTENSION_NAME);
    if (a.hasSpaceWarp) {
        enable.push_back(XR_FB_SPACE_WARP_EXTENSION_NAME);
    }
    // Meta Quest Super Resolution, through the eye layer's filter settings.
    a.hasLayerSettings = hasExtension(exts, XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    if (a.hasLayerSettings) {
        enable.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    }
    // Meta's dynamic resolution: the runtime recommends the eye size from the
    // GPU's load (and it is what lets Quest 3 raise the GPU to level 5).
    a.hasRecommendedRes = hasExtension(exts, XR_META_RECOMMENDED_LAYER_RESOLUTION_EXTENSION_NAME);
    if (a.hasRecommendedRes) {
        enable.push_back(XR_META_RECOMMENDED_LAYER_RESOLUTION_EXTENSION_NAME);
    }
    a.hasPerfMetrics = hasExtension(exts, XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);
    if (a.hasPerfMetrics) {
        enable.push_back(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);
    }
    // Passthrough, for the room around the giant screen (the runtime only
    // lists it with com.oculus.feature.PASSTHROUGH in the manifest).
    a.hasPassthrough = hasExtension(exts, XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (a.hasPassthrough) {
        enable.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    }
    std::string names;
    for (const auto& e : exts) {
        names += ' ';
        names += e.extensionName;
    }
    for (size_t i = 0; i < names.size(); i += 900) {
        port_log("XR runtime extensions:%s", names.substr(i, 900).c_str());
    }

    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = a.android->activity->vm;
    androidInfo.applicationActivity = a.android->activity->clazz;

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &androidInfo;
    strcpy(ci.applicationInfo.applicationName, "GalaxyQuest");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "Petari");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = (uint32_t)enable.size();
    ci.enabledExtensionNames = enable.data();
    XR_CHECK(xrCreateInstance(&ci, &a.instance));

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(a.instance, &sgi, &a.system));

    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    XrSystemSpaceWarpPropertiesFB warpProps{XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB};
    if (a.hasSpaceWarp) {
        props.next = &warpProps;
    }
    XrSystemPassthroughProperties2FB passProps{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB};
    if (a.hasPassthrough) {
        passProps.next = props.next;
        props.next = &passProps;
    }
    XR_CHECK(xrGetSystemProperties(a.instance, a.system, &props));
    if (a.hasPassthrough) {
        a.hasPassthrough = (passProps.capabilities & XR_PASSTHROUGH_CAPABILITY_BIT_FB) != 0;
        port_log("XR passthrough: %s (capabilities 0x%x)", a.hasPassthrough ? "available" : "not on this headset", (unsigned)passProps.capabilities);
    } else {
        port_log("XR passthrough: no extension");
    }
    port_log("XR system: %s (touch plus ext: %d, refresh ext: %d, performance settings ext: %d)", props.systemName, a.hasTouchPlus, a.hasRefreshRate,
             a.hasPerfSettings);
    if (a.hasSpaceWarp) {
        a.motionW = warpProps.recommendedMotionVectorImageRectWidth;
        a.motionH = warpProps.recommendedMotionVectorImageRectHeight;
        a.hasSpaceWarp = a.motionW > 0 && a.motionH > 0;
        port_log("XR SpaceWarp: motion vector images %ux%u (%s)", a.motionW, a.motionH, vr::spaceWarp() ? "on" : "off: space_warp = 0");
    } else {
        port_log("XR SpaceWarp: not available");
    }

    PFN_xrGetOpenGLESGraphicsRequirementsKHR getReq = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(a.instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq));
    XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    XR_CHECK(getReq(a.instance, a.system, &req));

    if (a.hasRefreshRate) {
        xrGetInstanceProcAddr(a.instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction*)&a.xrRequestDisplayRefreshRateFB);
        xrGetInstanceProcAddr(a.instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction*)&a.xrEnumerateDisplayRefreshRatesFB);
        xrGetInstanceProcAddr(a.instance, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction*)&a.xrGetDisplayRefreshRateFB);
    }
    if (a.hasPerfSettings) {
        xrGetInstanceProcAddr(a.instance, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction*)&a.xrPerfSettingsSetPerformanceLevelEXT);
    }
    if (a.hasRecommendedRes) {
        xrGetInstanceProcAddr(a.instance, "xrGetRecommendedLayerResolutionMETA", (PFN_xrVoidFunction*)&a.xrGetRecommendedLayerResolutionMETA);
    }
    if (a.hasPassthrough) {
        xrGetInstanceProcAddr(a.instance, "xrCreatePassthroughFB", (PFN_xrVoidFunction*)&a.xrCreatePassthroughFB);
        xrGetInstanceProcAddr(a.instance, "xrPassthroughStartFB", (PFN_xrVoidFunction*)&a.xrPassthroughStartFB);
        xrGetInstanceProcAddr(a.instance, "xrPassthroughPauseFB", (PFN_xrVoidFunction*)&a.xrPassthroughPauseFB);
        xrGetInstanceProcAddr(a.instance, "xrCreatePassthroughLayerFB", (PFN_xrVoidFunction*)&a.xrCreatePassthroughLayerFB);
        xrGetInstanceProcAddr(a.instance, "xrPassthroughLayerResumeFB", (PFN_xrVoidFunction*)&a.xrPassthroughLayerResumeFB);
        xrGetInstanceProcAddr(a.instance, "xrPassthroughLayerPauseFB", (PFN_xrVoidFunction*)&a.xrPassthroughLayerPauseFB);
        a.hasPassthrough = a.xrCreatePassthroughFB && a.xrPassthroughStartFB && a.xrPassthroughPauseFB && a.xrCreatePassthroughLayerFB &&
                           a.xrPassthroughLayerResumeFB && a.xrPassthroughLayerPauseFB;
    }
    vr::setPassthroughAvailable(a.hasPassthrough);
    if (a.hasPerfMetrics) {
        xrGetInstanceProcAddr(a.instance, "xrEnumeratePerformanceMetricsCounterPathsMETA",
                              (PFN_xrVoidFunction*)&a.xrEnumeratePerformanceMetricsCounterPathsMETA);
        xrGetInstanceProcAddr(a.instance, "xrSetPerformanceMetricsStateMETA", (PFN_xrVoidFunction*)&a.xrSetPerformanceMetricsStateMETA);
        xrGetInstanceProcAddr(a.instance, "xrQueryPerformanceMetricsCounterMETA", (PFN_xrVoidFunction*)&a.xrQueryPerformanceMetricsCounterMETA);
        a.hasPerfMetrics = a.xrEnumeratePerformanceMetricsCounterPathsMETA && a.xrSetPerformanceMetricsStateMETA && a.xrQueryPerformanceMetricsCounterMETA;
    }
    port_log("XR layer settings (Super Resolution) ext: %d, recommended layer resolution ext: %d", a.hasLayerSettings, a.hasRecommendedRes);
}

// Asks for Meta's SustainedHigh CPU and GPU level range (the high_clocks
// setting; on by default).  Without it the system keeps the CPU as low as
// its utilization allows: level 2 (1.38 GHz on Quest 3) in gameplay, which
// stretched the frame loop's own work until it missed refreshes.
void requestPerformanceLevels(App& a) {
    if (!a.xrPerfSettingsSetPerformanceLevelEXT || !vr::highClocks()) {
        return;
    }
    XrResult cpu = a.xrPerfSettingsSetPerformanceLevelEXT(a.session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
    XrResult gpu = a.xrPerfSettingsSetPerformanceLevelEXT(a.session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
    port_log("performance levels: CPU and GPU sustained high requested (%d, %d)", (int)cpu, (int)gpu);
}

// Asks for the refresh rate in the settings (120 Hz by default: a whole two
// refreshes per 60 Hz game frame), falling back to 72 Hz.
void requestRefreshRate(App& a) {
    if (!a.xrRequestDisplayRefreshRateFB) {
        return;
    }
    std::string list;
    if (a.xrEnumerateDisplayRefreshRatesFB) {
        uint32_t n = 0;
        if (XR_SUCCEEDED(a.xrEnumerateDisplayRefreshRatesFB(a.session, 0, &n, nullptr)) && n > 0) {
            std::vector<float> rates(n);
            a.xrEnumerateDisplayRefreshRatesFB(a.session, n, &n, rates.data());
            vr::setRefreshRates(rates.data(), (int)n);  // for the settings panel
            for (float r : rates) {
                char buf[16];
                snprintf(buf, sizeof(buf), " %.0f", r);
                list += buf;
            }
        }
    }
    float want = vr::refreshRate();
    a.requestedHz = want;
    XrResult r = a.xrRequestDisplayRefreshRateFB(a.session, want);
    port_log("display refresh rates:%s Hz; requested %.0f Hz: %d", list.c_str(), want, (int)r);
    if (XR_FAILED(r)) {
        a.xrRequestDisplayRefreshRateFB(a.session, 72.0f);
    }
    // The rate now (a change arrives as an event).
    if (a.xrGetDisplayRefreshRateFB && XR_SUCCEEDED(a.xrGetDisplayRefreshRateFB(a.session, &a.displayHz))) {
        port_log("display refresh rate now %.0f Hz", a.displayHz);
    }
}

// The headset's passthrough runs while the passthrough setting wants the
// room shown (and until the room has faded out again).  Meta's docs: a
// paused layer is not submitted, and pausing is how to hide one.
void updatePassthrough(App& a) {
    if (!a.hasPassthrough) {
        return;
    }
    bool want = vr::passthroughWanted() || vr::passthroughShown() > 0.0f;
    if (want == a.passthroughRunning) {
        return;
    }
    if (want) {
        if (a.passthrough == XR_NULL_HANDLE) {
            XrPassthroughCreateInfoFB ci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
            XrResult r = a.xrCreatePassthroughFB(a.session, &ci, &a.passthrough);
            XrPassthroughLayerCreateInfoFB li{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
            li.passthrough = a.passthrough;
            li.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
            XrResult rl = XR_SUCCEEDED(r) ? a.xrCreatePassthroughLayerFB(a.session, &li, &a.passthroughLayer) : r;
            port_log("XR passthrough: feature and layer created (%d, %d)", (int)r, (int)rl);
            if (XR_FAILED(r) || XR_FAILED(rl)) {
                a.hasPassthrough = false;
                vr::setPassthroughAvailable(false);
                return;
            }
        }
        XrResult r = a.xrPassthroughStartFB(a.passthrough);
        XrResult rl = a.xrPassthroughLayerResumeFB(a.passthroughLayer);
        port_log("XR passthrough: started (%d, %d)", (int)r, (int)rl);
        if (XR_FAILED(r) || XR_FAILED(rl)) {
            a.hasPassthrough = false;
            vr::setPassthroughAvailable(false);
            return;
        }
    } else {
        XrResult rl = a.xrPassthroughLayerPauseFB(a.passthroughLayer);
        XrResult r = a.xrPassthroughPauseFB(a.passthrough);
        port_log("XR passthrough: paused (%d, %d)", (int)r, (int)rl);
    }
    a.passthroughRunning = want;
}

XrAction makeAction(App& a, const char* name, XrActionType type, bool bothHands) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    strcpy(ci.actionName, name);
    strcpy(ci.localizedActionName, name);
    if (bothHands) {
        ci.countSubactionPaths = 2;
        ci.subactionPaths = a.handPath;
    }
    XrAction act;
    XR_CHECK(xrCreateAction(a.actionSet, &ci, &act));
    return act;
}

XrPath path(App& a, const char* s) {
    XrPath p;
    XR_CHECK(xrStringToPath(a.instance, s, &p));
    return p;
}

void suggest(App& a, const char* profile, bool touchPlus) {
    std::vector<XrActionSuggestedBinding> b = {
        {a.moveAction, path(a, "/user/hand/left/input/thumbstick")},
        {a.lookAction, path(a, "/user/hand/right/input/thumbstick")},
        {a.aAction, path(a, "/user/hand/right/input/a/click")},
        {a.bAction, path(a, "/user/hand/right/input/b/click")},
        {a.xAction, path(a, "/user/hand/left/input/x/click")},
        {a.yAction, path(a, "/user/hand/left/input/y/click")},
        {a.triggerAction, path(a, "/user/hand/left/input/trigger/value")},
        {a.triggerAction, path(a, "/user/hand/right/input/trigger/value")},
        {a.gripAction, path(a, "/user/hand/left/input/squeeze/value")},
        {a.gripAction, path(a, "/user/hand/right/input/squeeze/value")},
        {a.menuAction, path(a, "/user/hand/left/input/menu/click")},
        {a.stickClickAction, path(a, "/user/hand/left/input/thumbstick/click")},
        {a.stickClickAction, path(a, "/user/hand/right/input/thumbstick/click")},
        {a.aimPoseAction, path(a, "/user/hand/left/input/aim/pose")},
        {a.aimPoseAction, path(a, "/user/hand/right/input/aim/pose")},
        {a.hapticAction, path(a, "/user/hand/left/output/haptic")},
        {a.hapticAction, path(a, "/user/hand/right/output/haptic")},
    };
    (void)touchPlus;
    XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    s.interactionProfile = path(a, profile);
    s.suggestedBindings = b.data();
    s.countSuggestedBindings = (uint32_t)b.size();
    XrResult r = xrSuggestInteractionProfileBindings(a.instance, &s);
    port_log("bindings for %s: %d", profile, (int)r);
}

void initActions(App& a) {
    a.handPath[0] = path(a, "/user/hand/left");
    a.handPath[1] = path(a, "/user/hand/right");
    XrActionSetCreateInfo ci{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(ci.actionSetName, "gameplay");
    strcpy(ci.localizedActionSetName, "Gameplay");
    XR_CHECK(xrCreateActionSet(a.instance, &ci, &a.actionSet));
    a.moveAction = makeAction(a, "move", XR_ACTION_TYPE_VECTOR2F_INPUT, false);
    a.lookAction = makeAction(a, "look", XR_ACTION_TYPE_VECTOR2F_INPUT, false);
    a.aAction = makeAction(a, "button_a", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    a.bAction = makeAction(a, "button_b", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    a.xAction = makeAction(a, "button_x", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    a.yAction = makeAction(a, "button_y", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    a.triggerAction = makeAction(a, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, true);
    a.gripAction = makeAction(a, "grip", XR_ACTION_TYPE_FLOAT_INPUT, true);
    a.menuAction = makeAction(a, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    a.stickClickAction = makeAction(a, "stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    a.aimPoseAction = makeAction(a, "aim_pose", XR_ACTION_TYPE_POSE_INPUT, true);
    a.hapticAction = makeAction(a, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);

    if (a.hasTouchPlus) {
        suggest(a, "/interaction_profiles/meta/touch_controller_plus", true);
    }
    suggest(a, "/interaction_profiles/oculus/touch_controller", false);
}

void initSession(App& a) {
    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = a.display;
    binding.config = a.config;
    binding.context = a.context;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = a.system;
    XR_CHECK(xrCreateSession(a.instance, &sci, &a.session));

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    XR_CHECK(xrCreateReferenceSpace(a.session, &rs, &a.appSpace));
    XR_CHECK(xrCreateReferenceSpace(a.session, &rs, &a.localSpace));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(a.session, &rs, &a.viewSpace));

    XrSessionActionSetsAttachInfo att{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    att.countActionSets = 1;
    att.actionSets = &a.actionSet;
    XR_CHECK(xrAttachSessionActionSets(a.session, &att));
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo asci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asci.action = a.aimPoseAction;
        asci.subactionPath = a.handPath[h];
        asci.poseInActionSpace.orientation.w = 1.0f;
        XR_CHECK(xrCreateActionSpace(a.session, &asci, &a.aimSpace[h]));
    }

    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(a.instance, a.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr));
    viewCount = viewCount > 2 ? 2 : viewCount;
    for (auto& v : a.viewConfig) {
        v.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    }
    XR_CHECK(xrEnumerateViewConfigurationViews(a.instance, a.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, a.viewConfig));

    uint32_t fmtCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(a.session, 0, &fmtCount, nullptr));
    std::vector<int64_t> fmts(fmtCount);
    XR_CHECK(xrEnumerateSwapchainFormats(a.session, fmtCount, &fmtCount, fmts.data()));
    int64_t format = fmts[0];
    for (int64_t f : fmts) {
        if (f == GL_SRGB8_ALPHA8) {
            format = f;
        }
    }

    // Eye images: the headset's recommended size times the swapchain scale
    // (the game renders at its dynamic resolution and the composite
    // resamples, see vr_game.cpp).
    float scale = vr::swapchainScale();
    for (int e = 0; e < 2; e++) {
        const XrViewConfigurationView& vc = a.viewConfig[e];
        int32_t w = (int32_t)lroundf(vc.recommendedImageRectWidth * scale);
        int32_t h = (int32_t)lroundf(vc.recommendedImageRectHeight * scale);
        w = w < (int32_t)vc.maxImageRectWidth ? w : (int32_t)vc.maxImageRectWidth;
        h = h < (int32_t)vc.maxImageRectHeight ? h : (int32_t)vc.maxImageRectHeight;
        for (int s = 0; s < 2; s++) {
            Swapchain& sc = a.eyes[s][e];
            sc.width = w;
            sc.height = h;
            XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            ci.format = format;
            ci.sampleCount = 1;
            ci.width = (uint32_t)sc.width;
            ci.height = (uint32_t)sc.height;
            ci.faceCount = 1;
            ci.arraySize = 1;
            ci.mipCount = 1;
            XR_CHECK(xrCreateSwapchain(a.session, &ci, &sc.handle));
            uint32_t n = 0;
            XR_CHECK(xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr));
            sc.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
            XR_CHECK(xrEnumerateSwapchainImages(sc.handle, n, &n, (XrSwapchainImageBaseHeader*)sc.images.data()));
            // Color only: the composite into the swapchain (vr::renderEye)
            // draws without depth.
            sc.fbos.resize(n);
            glGenFramebuffers((GLsizei)n, sc.fbos.data());
            for (uint32_t i = 0; i < n; i++) {
                glBindFramebuffer(GL_FRAMEBUFFER, sc.fbos[i]);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc.images[i].image, 0);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        port_log("eye %d swapchains %dx%d (recommended %ux%u, max %ux%u), format 0x%llx", e, w, h, vc.recommendedImageRectWidth,
                 vc.recommendedImageRectHeight, vc.maxImageRectWidth, vc.maxImageRectHeight, (long long)format);
    }

    // SpaceWarp: per eye, motion vectors (16-bit float, as Meta recommends)
    // and depth with stencil, at the recommended size.
    if (a.hasSpaceWarp) {
        bool haveMotion = false, haveDepth = false;
        for (int64_t f : fmts) {
            haveMotion |= f == GL_RGBA16F;
            haveDepth |= f == GL_DEPTH24_STENCIL8;
        }
        if (!haveMotion || !haveDepth) {
            port_log("XR SpaceWarp: swapchain formats missing (RGBA16F %d, DEPTH24_STENCIL8 %d): off", haveMotion, haveDepth);
            a.hasSpaceWarp = false;
        }
    }
    if (a.hasSpaceWarp) {
        for (int e = 0; e < 2; e++) {
            for (int k = 0; k < 2; k++) {
                Swapchain& sc = k == 0 ? a.motion[e] : a.depth[e];
                sc.width = (int32_t)a.motionW;
                sc.height = (int32_t)a.motionH;
                XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                                (k == 0 ? XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT : XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
                ci.format = k == 0 ? GL_RGBA16F : GL_DEPTH24_STENCIL8;
                ci.sampleCount = 1;
                ci.width = a.motionW;
                ci.height = a.motionH;
                ci.faceCount = 1;
                ci.arraySize = 1;
                ci.mipCount = 1;
                XR_CHECK(xrCreateSwapchain(a.session, &ci, &sc.handle));
                uint32_t n = 0;
                XR_CHECK(xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr));
                sc.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
                XR_CHECK(xrEnumerateSwapchainImages(sc.handle, n, &n, (XrSwapchainImageBaseHeader*)sc.images.data()));
            }
        }
        vr::setMotionSize((int)a.motionW, (int)a.motionH);
        port_log("XR SpaceWarp: motion vector and depth swapchains %ux%u, %zu images each", a.motionW, a.motionH, a.motion[0].images.size());
    }
}

// The panels with text (the HUD panel, the settings panel) as quad layers
// over the eye layer: one swapchain each at the panel's own size (after
// vr::init, which sizes them).
void initUiLayers(App& a) {
    uint32_t fmtCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(a.session, 0, &fmtCount, nullptr));
    std::vector<int64_t> fmts(fmtCount);
    XR_CHECK(xrEnumerateSwapchainFormats(a.session, fmtCount, &fmtCount, fmts.data()));
    bool srgb = false;
    for (int64_t f : fmts) srgb |= f == GL_SRGB8_ALPHA8;
    if (!srgb) {
        port_log("XR UI layers: no sRGB swapchain format; the panels stay in the eye images");
        return;
    }
    for (int k = 0; k < vr::kUiLayerCount; k++) {
        Swapchain& sc = a.ui[k];
        vr::uiLayerSize(k, &sc.width, &sc.height);
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = GL_SRGB8_ALPHA8;
        ci.sampleCount = 1;
        ci.width = (uint32_t)sc.width;
        ci.height = (uint32_t)sc.height;
        ci.faceCount = 1;
        ci.arraySize = 1;
        // The virtual screen is often seen smaller than its picture (a
        // distant giant screen): with mipmaps the compositor filters it down
        // instead of skipping texels, which shimmered on fine detail.
        sc.mips = 1;
        if (k == vr::kScreenLayer || k == vr::kScreenRightLayer) {
            while ((std::max(sc.width, sc.height) >> sc.mips) > 0) sc.mips++;
        }
        ci.mipCount = sc.mips;
        XR_CHECK(xrCreateSwapchain(a.session, &ci, &sc.handle));
        uint32_t n = 0;
        XR_CHECK(xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr));
        sc.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        XR_CHECK(xrEnumerateSwapchainImages(sc.handle, n, &n, (XrSwapchainImageBaseHeader*)sc.images.data()));
        sc.fbos.resize(n);
        glGenFramebuffers((GLsizei)n, sc.fbos.data());
        for (uint32_t i = 0; i < n; i++) {
            glBindFramebuffer(GL_FRAMEBUFFER, sc.fbos[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc.images[i].image, 0);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        port_log("XR UI layer %d: swapchain %dx%d, %u mip levels, %u images", k, sc.width, sc.height, sc.mips, n);
    }
    vr::setUiLayers(true);
}

// Turns the runtime's performance counters on and finds the ones used.
void initPerfMetrics(App& a) {
    if (!a.hasPerfMetrics) {
        port_log("XR performance metrics: not available");
        return;
    }
    XrPerformanceMetricsStateMETA state{XR_TYPE_PERFORMANCE_METRICS_STATE_META};
    state.enabled = XR_TRUE;
    if (XR_FAILED(a.xrSetPerformanceMetricsStateMETA(a.session, &state))) {
        port_log("XR performance metrics: could not be turned on");
        a.hasPerfMetrics = false;
        return;
    }
    uint32_t n = 0;
    a.xrEnumeratePerformanceMetricsCounterPathsMETA(a.instance, 0, &n, nullptr);
    std::vector<XrPath> paths(n);
    if (n) a.xrEnumeratePerformanceMetricsCounterPathsMETA(a.instance, n, &n, paths.data());
    std::string names;
    for (XrPath p : paths) {
        char name[XR_MAX_PATH_LENGTH];
        uint32_t len = 0;
        if (XR_FAILED(xrPathToString(a.instance, p, sizeof(name), &len, name))) continue;
        names += ' ';
        names += name;
        if (!strcmp(name, "/perfmetrics_meta/app/gpu_frametime")) a.appGpuPath = p;
        if (!strcmp(name, "/perfmetrics_meta/compositor/gpu_frametime")) a.compositorGpuPath = p;
        if (!strcmp(name, "/perfmetrics_meta/device/gpu_utilization")) a.gpuUtilPath = p;
    }
    port_log("XR performance metrics: %u counters:%s", n, names.c_str());
}

// A counter's value (-1 when unknown).
float queryCounter(App& a, XrPath path, XrPerformanceMetricsCounterUnitMETA* unit = nullptr) {
    if (path == XR_NULL_PATH) return -1.0f;
    XrPerformanceMetricsCounterMETA c{XR_TYPE_PERFORMANCE_METRICS_COUNTER_META};
    if (XR_FAILED(a.xrQueryPerformanceMetricsCounterMETA(a.session, path, &c))) return -1.0f;
    if (unit) *unit = c.counterUnit;
    if (c.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META) return c.floatValue;
    if (c.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META) return (float)c.uintValue;
    return -1.0f;
}

// ---------------------------------------------------------------------------
// Input -> emulated Wii remote + Nunchuk
// ---------------------------------------------------------------------------
bool getBool(App& a, XrAction act, XrPath sub = XR_NULL_PATH) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = act;
    gi.subactionPath = sub;
    XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
    xrGetActionStateBoolean(a.session, &gi, &s);
    return s.isActive && s.currentState;
}

float getFloat(App& a, XrAction act, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = act;
    gi.subactionPath = sub;
    XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
    xrGetActionStateFloat(a.session, &gi, &s);
    return s.isActive ? s.currentState : 0.0f;
}

XrVector2f getVec2(App& a, XrAction act) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = act;
    XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
    xrGetActionStateVector2f(a.session, &gi, &s);
    return s.isActive ? s.currentState : XrVector2f{0, 0};
}

void updateInput(App& a, XrTime time) {
    XrActiveActionSet active{a.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(a.session, &sync))) {
        return;
    }

    enum : uint32_t { W_LEFT = 0x0001, W_RIGHT = 0x0002, W_DOWN = 0x0004, W_UP = 0x0008, W_PLUS = 0x0010, W_2 = 0x0100, W_1 = 0x0200,
                      W_B = 0x0400, W_A = 0x0800, W_MINUS = 0x1000, W_Z = 0x2000, W_C = 0x4000, W_HOME = 0x8000 };

    PortPadState pad{};
    pad.connected = 1;
    XrVector2f move = getVec2(a, a.moveAction);
    pad.stickX = move.x;
    pad.stickY = move.y;
    // Button layout (see docs/CONTROLS.md):
    //   A = jump, right trigger = shoot star bits (B), B, Y or controller shake = spin,
    //   left trigger = crouch (Z), left grip = camera centre (C), menu = pause (+),
    //   X = minus, right stick = D-pad, right stick click = first-person look (D-pad up).
    if (getBool(a, a.aAction)) pad.buttons |= W_A;
    if (getFloat(a, a.triggerAction, a.handPath[1]) > 0.5f) pad.buttons |= W_B;
    if (getFloat(a, a.triggerAction, a.handPath[0]) > 0.5f) pad.buttons |= W_Z;
    if (getFloat(a, a.gripAction, a.handPath[0]) > 0.6f) pad.buttons |= W_C;
    // Menu and X are + and -; a press of either also asks for the pause
    // menu (port_input_set), which the game opens at once or as soon as it
    // allows it: on the Wii + or - had to be held for 12 frames, with no A,
    // B or shake.
    if (getBool(a, a.menuAction)) pad.buttons |= W_PLUS;
    if (getBool(a, a.xAction)) pad.buttons |= W_MINUS;
    if (getBool(a, a.stickClickAction, a.handPath[1])) pad.buttons |= W_UP;
    // Right stick: the D-pad.  In the diorama left/right turn it in steps
    // round Mario (a snap turn behind a blink; its yaw no longer follows the
    // game camera); elsewhere they turn the game camera.  With the
    // invert_camera setting both turn the other way (the game swaps the
    // D-pad's sides for its camera, CameraLocalUtil.cpp): pushing the stick
    // right turns the view to the right.  Up is the first-person view.  A
    // direction engages past 0.7 and releases below 0.4, one press per flick.
    XrVector2f look = getVec2(a, a.lookAction);
    float lx = fabsf(look.x), ly = fabsf(look.y);
    if (a.dpad == 0) {
        if (lx > 0.7f && lx >= ly) {
            a.dpad = look.x < 0.0f ? W_LEFT : W_RIGHT;
            a.dpadTurned = vr::snapTurn((look.x < 0.0f ? -1 : 1) * (vr::invertCamera() ? -1 : 1));
        } else if (ly > 0.7f) {
            a.dpad = look.y > 0.0f ? W_UP : W_DOWN;
        }
    } else if (fmaxf(lx, ly) < 0.4f) {
        a.dpad = 0;
        a.dpadTurned = false;
    }
    if (!a.dpadTurned) pad.buttons |= a.dpad;

    // Spin: B or Y, or flicking the right controller like a Wii Remote,
    // produces the acceleration spike the game reads as a remote shake (some
    // objects only answer that one); flicking the left controller shakes the
    // Nunchuk, which also spins.
    static int spinFrames = 0, nunSpinFrames = 0;
    static XrTime lastFlick[2] = {0, 0};
    if (getBool(a, a.bAction) || getBool(a, a.yAction)) {
        spinFrames = 6;
    }
    for (int h = 0; h < 2; h++) {
        XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation vloc{XR_TYPE_SPACE_LOCATION};
        vloc.next = &vel;
        if (XR_SUCCEEDED(xrLocateSpace(a.aimSpace[h], a.appSpace, time, &vloc)) && (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)) {
            const XrVector3f& v = vel.linearVelocity;
            float speed = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
            if (speed > 2.5f && time - lastFlick[h] > 300000000) {  // m/s; 0.3 s between spins
                (h == 1 ? spinFrames : nunSpinFrames) = 6;
                lastFlick[h] = time;
            }
        }
    }
    if (spinFrames > 0) {
        float s = (spinFrames & 1) ? 2.5f : -2.5f;
        pad.accX = s;
        pad.accY = s;
        spinFrames--;
    }
    if (nunSpinFrames > 0) {
        float s = (nunSpinFrames & 1) ? 2.5f : -2.5f;
        pad.nunAccX = s;
        pad.nunAccY = s;
        nunSpinFrames--;
    }

    // Pointer: where the right controller's aim ray meets the HUD panel (in
    // the diorama) or the virtual screen (menus).  While it is on the VR
    // settings panel beside the pause menu, the game gets no pointer, and an
    // A press made there goes to the panel.  Which way is down from the
    // controller tilts the Wii remote where the game steers by tilt.
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    xm::Vec3 aimFrom{0, 0, 0}, aimDir{0, 0, 0};
    bool aimOriented = XR_SUCCEEDED(xrLocateSpace(a.aimSpace[1], a.appSpace, time, &loc)) &&
                       (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    bool aimTracked = aimOriented && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    xm::Quat aimQ{loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w};
    if (aimOriented) {
        const xm::Vec3 down{0.0f, -1.0f, 0.0f};  // the app space is gravity aligned
        pad.tilted = 1;
        pad.downX = xm::dot(down, xm::rotate(aimQ, {1, 0, 0}));
        pad.downY = xm::dot(down, xm::rotate(aimQ, {0, 1, 0}));
        pad.downZ = xm::dot(down, xm::rotate(aimQ, {0, 0, -1}));
    }
    if (aimTracked) {
        aimFrom = {loc.pose.position.x, loc.pose.position.y, loc.pose.position.z};
        aimDir = xm::rotate(aimQ, {0, 0, -1});
        pad.pointerValid = vr::pointerFromRay(aimFrom, aimDir, &pad.pointerX, &pad.pointerY);
    }
    if (aimTracked != a.aimTracked) {
        // A controller that is asleep, set down or out of the cameras' view:
        // the laser goes (rather than freezing where it was) and the game's
        // pointer leaves the screen until it is back.
        a.aimTracked = aimTracked;
        port_log("vr: right controller aim %s", aimTracked ? "tracked again" : "lost");
        if (!aimTracked) {
            vr::pointerLost();
        }
    }
    if (gScriptCount > 0) {
        portApplyInputScript(gScript, gScriptCount, (int)((port_host_time_ns() - gBootNs) / 1000000), &pad);
    }
    // PETARI_VRAIM="t0-t1:x,y,z[:A];...": from t0 to t1 s after boot the aim
    // comes from a controller held low and to the right, towards stage
    // point x,y,z, with A held for :A (tests of the settings panel).
    if (const char* spec = getenv("PETARI_VRAIM")) {
        double s = (port_host_time_ns() - gBootNs) / 1e9;
        for (const char* p = spec; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : nullptr) {
            float t0, t1, x, y, z;
            char btn = 0;
            int n = sscanf(p, "%f-%f:%f,%f,%f:%c", &t0, &t1, &x, &y, &z, &btn);
            if (n >= 5 && s >= t0 && s < t1) {
                aimFrom = {0.22f, -0.35f, -0.25f};
                aimDir = xm::normalize(xm::Vec3{x, y, z} - aimFrom);
                pad.pointerValid = vr::pointerFromRay(aimFrom, aimDir, &pad.pointerX, &pad.pointerY);
                if (n == 6 && btn == 'A') pad.buttons |= W_A;
            }
        }
    }
    // The setup screen (no game yet) takes the pointer and every click.
    if (vr::setupActive()) {
        float onSetup = vr::setupPointer(aimFrom, aimDir, (pad.buttons & (W_A | W_B)) != 0);
        if (onSetup > 0.0f) {
            vr::setAimLength(onSetup);
        }
        pad.pointerValid = 0;
        pad.buttons = 0;
    }
    // The settings panel takes clicks made with A or the trigger (B).
    float onPanel = vr::settingsPointer(aimFrom, aimDir, (pad.buttons & (W_A | W_B)) != 0);
    if (onPanel > 0.0f) {
        pad.pointerValid = 0;
        vr::setAimLength(onPanel);
    }
    if (vr::settingsOwnsClick()) {
        pad.buttons &= ~(W_A | W_B);
    }
    port_input_set(0, &pad);

    // Rumble -> Touch Plus haptics.
    int rumble = port_input_rumble(0);
    if (rumble != a.lastRumble) {
        for (int h = 0; h < 2; h++) {
            XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
            hi.action = a.hapticAction;
            hi.subactionPath = a.handPath[h];
            if (rumble) {
                XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
                v.amplitude = 0.6f;
                v.duration = XR_INFINITE_DURATION;
                v.frequency = XR_FREQUENCY_UNSPECIFIED;
                xrApplyHapticFeedback(a.session, &hi, (XrHapticBaseHeader*)&v);
            } else {
                xrStopHapticFeedback(a.session, &hi);
            }
        }
        a.lastRumble = rumble;
    }
    // A light tick when the pointer touches a new target or a control of the
    // settings panel (while no rumble).
    bool tick = vr::takePointerTouch();
    tick = vr::settingsTakeTick() || tick;
    tick = vr::setupTakeTick() || tick;
    if (tick && rumble == 0) {
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
        hi.action = a.hapticAction;
        hi.subactionPath = a.handPath[1];
        XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
        v.amplitude = 0.25f;
        v.duration = 15000000;  // 15 ms
        v.frequency = XR_FREQUENCY_UNSPECIFIED;
        xrApplyHapticFeedback(a.session, &hi, (XrHapticBaseHeader*)&v);
    }
}

// The game only runs while the session has input focus.  Coming back to
// gameplay opens the game's pause menu, as the Wii's HOME menu did.
void updateFocus(App& a) {
    bool focused = a.state == XR_SESSION_STATE_FOCUSED;
    if (focused == a.focused) {
        return;
    }
    a.focused = focused;
    if (!focused) {
        uint32_t flags = gpu::renderer().camera().flags;
        a.pausedInGameplay = (flags & PORT_GX_CAMERA_DIORAMA) && !(flags & PORT_GX_CAMERA_POINTER_UI);
        if (a.sessionRunning) {
            for (int h = 0; h < 2; h++) {
                XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
                hi.action = a.hapticAction;
                hi.subactionPath = a.handPath[h];
                xrStopHapticFeedback(a.session, &hi);
            }
        }
        a.lastRumble = 0;
    } else if (a.pausedInGameplay) {
        port_input_request_pause();  // back from the system menu: the game's pause menu
        a.pausedInGameplay = false;
    }
    port_set_paused(!focused);
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------
void handleEvents(App& a, bool& quit) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(a.instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* sc = (XrEventDataSessionStateChanged*)&ev;
            a.state = sc->state;
            port_log("XR session state -> %d", (int)a.state);
            updateFocus(a);
            if (a.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XR_CHECK(xrBeginSession(a.session, &bi));
                a.sessionRunning = true;
                a.facePlayerDue = true;
                a.faceWatchStart = true;
                requestRefreshRate(a);
                requestPerformanceLevels(a);
            } else if (a.state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(a.session);
                a.sessionRunning = false;
            } else if (a.state == XR_SESSION_STATE_EXITING || a.state == XR_SESSION_STATE_LOSS_PENDING) {
                quit = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            quit = true;
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            // The player recentred the view (the Meta button): the screen
            // goes where they face now, once LOCAL has moved.
            auto* rc = (XrEventDataReferenceSpaceChangePending*)&ev;
            port_log("vr: reference space %d changes", (int)rc->referenceSpaceType);
            if (rc->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                a.facePlayerDue = true;
                a.facePlayerAfter = rc->changeTime;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            auto* rc = (XrEventDataDisplayRefreshRateChangedFB*)&ev;
            port_log("display refresh rate %.0f -> %.0f Hz", rc->fromDisplayRefreshRate, rc->toDisplayRefreshRate);
            a.displayHz = rc->toDisplayRefreshRate;
            break;
        }
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
            // Controllers put down (hands take over) or picked up again.
            for (int h = 0; h < 2; h++) {
                XrInteractionProfileState ps{XR_TYPE_INTERACTION_PROFILE_STATE};
                char path[XR_MAX_PATH_LENGTH] = "none";
                uint32_t n = 0;
                if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(a.session, a.handPath[h], &ps)) && ps.interactionProfile != XR_NULL_PATH) {
                    xrPathToString(a.instance, ps.interactionProfile, sizeof(path), &n, path);
                }
                port_log("vr: %s hand input profile: %s", h ? "right" : "left", path);
            }
            break;
        }
        default:
            break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// The app's space puts the screen (and the diorama) straight ahead of where
// the player is when a session starts: at launch, and when the headset is
// put back on.  The runtime's LOCAL space alone starts at the pose of the
// player's last recentring, which may point anywhere by the time the game
// starts: the screen came up behind the player.  So on a session's first
// frame with a tracked head pose, the app's space becomes LOCAL moved to the
// head and turned to where it faces (about the vertical only: the floor stays
// level).  The same when the player recentres with the Meta button, once
// LOCAL has moved, and when the head's pose jumps in a session's first
// seconds: a headset just woken up may still be finding its place in the
// room, and takes LOCAL's contents with it when it does.
void setAppSpace(App& a, const XrPosef& poseInLocal) {
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace = poseInLocal;
    XrSpace space = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateReferenceSpace(a.session, &rs, &space))) {
        return;
    }
    xrDestroySpace(a.appSpace);
    a.appSpace = space;
}

void dropHeldEye(App& a);

void updateAppSpace(App& a, XrTime time) {
    if (a.faceWatchStart) {
        a.faceWatchStart = false;
        a.faceWatchUntil = time + 5000000000ll;
        a.lastHeadValid = false;
    }
    if (!a.facePlayerDue && time >= a.faceWatchUntil) {
        return;
    }
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                      XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    if (XR_FAILED(xrLocateSpace(a.viewSpace, a.localSpace, time, &loc)) || (loc.locationFlags & need) != need) {
        a.lastHeadValid = false;
        return;  // not tracking yet: the next frame
    }
    // The turn about the vertical, from the head's right axis (level however
    // far the player looks up or down).
    xm::Quat q{loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w};
    xm::Vec3 right = xm::rotate(q, {1.0f, 0.0f, 0.0f});
    float yaw = atan2f(-right.z, right.x);
    // No head turns 30 degrees or moves half a metre from one frame to the
    // next: the tracking jumped.
    if (!a.facePlayerDue && a.lastHeadValid) {
        const XrVector3f& p = loc.pose.position;
        float turned = fabsf(remainderf(yaw - a.lastHeadYaw, 6.2831853f));
        float moved = sqrtf((p.x - a.lastHeadPos.x) * (p.x - a.lastHeadPos.x) + (p.y - a.lastHeadPos.y) * (p.y - a.lastHeadPos.y) +
                            (p.z - a.lastHeadPos.z) * (p.z - a.lastHeadPos.z));
        if (turned > 0.52f || moved > 0.5f) {
            port_log("vr: the head's pose jumped %.0f deg and %.2f m in a frame: tracking settled", turned * 57.29578f, moved);
            a.facePlayerDue = true;
        }
    }
    a.lastHeadValid = true;
    a.lastHeadYaw = yaw;
    a.lastHeadPos = loc.pose.position;
    if (!a.facePlayerDue || time < a.facePlayerAfter) {
        return;
    }
    a.facePlayerDue = false;
    XrPosef pose{};
    pose.orientation = {0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f)};
    pose.position = loc.pose.position;
    setAppSpace(a, pose);
    dropHeldEye(a);
    // The head in the new space, for the log: straight ahead at its origin.
    float now = 0.0f;
    XrSpaceLocation check{XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(xrLocateSpace(a.viewSpace, a.appSpace, time, &check)) && (check.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        xm::Quat c{check.pose.orientation.x, check.pose.orientation.y, check.pose.orientation.z, check.pose.orientation.w};
        xm::Vec3 r = xm::rotate(c, {1.0f, 0.0f, 0.0f});
        now = atan2f(-r.z, r.x) * 57.29578f;
    }
    port_log("vr: the view starts where the player faces: %.0f deg round from the runtime's own, head at %.2f %.2f %.2f m there (now %.1f deg off "
             "straight ahead)",
             yaw * 57.29578f, pose.position.x, pose.position.y, pose.position.z, now);
}

// Eye poses and fields of view at `time`.
void locateViews(App& a, XrTime time, XrView views[2]) {
    views[0] = {XR_TYPE_VIEW};
    views[1] = {XR_TYPE_VIEW};
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = time;
    li.space = a.appSpace;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    XR_CHECK(xrLocateViews(a.session, &li, &vs, 2, &n, views));
}

vr::EyeInfo eyeInfo(const App& a, const XrView& v, int e) {
    vr::EyeInfo eye;
    eye.proj = xm::projectionFov(v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp, v.fov.angleDown, vr::kNearZ, vr::kFarZ);
    xm::Quat q{v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w};
    xm::Vec3 p{v.pose.position.x, v.pose.position.y, v.pose.position.z};
    eye.view = xm::inversePose(q, p);
    eye.position = p;
    eye.orientation = q;
    eye.width = a.eyes[0][e].width;
    eye.height = a.eyes[0][e].height;
    return eye;
}

vr::FrameInfo frameInfo(const App& a, const XrView views[2], XrTime time) {
    vr::FrameInfo frame{};
    frame.time = (double)time * 1e-9;
    for (int e = 0; e < 2; e++) {
        const XrFovf& f = views[e].fov;
        float tx = fmaxf(fabsf(tanf(f.angleLeft)), fabsf(tanf(f.angleRight)));
        float ty = fmaxf(fabsf(tanf(f.angleUp)), fabsf(tanf(f.angleDown)));
        frame.cullTanX = fmaxf(frame.cullTanX, tx);
        frame.cullTanY = fmaxf(frame.cullTanY, ty);
        frame.eyes[e] = eyeInfo(a, views[e], e);
    }
    return frame;
}

// PETARI_VRSHOT_MS=<ms> (petari_debug.env): every that many ms, both eyes
// of a rendered set are saved side by side, at half size, to
// <files>/vrshots/shot_NNN_fFRAME.png, to see the headset's picture from a
// PC; with SpaceWarp, the left eye's motion vectors go to
// shot_NNN_fFRAME_mv.png (see vr::motionDebugImage), and the virtual
// screen's layers, which are not in the eye images, to
// shot_NNN_fFRAME_screen.png (the next picture drawn for them; a stereo
// pair as _screenL.png and _screenR.png).  The read-back stalls the frame
// loop a little each time.
std::string gShotDir;
int64_t gShotIntervalNs = 0, gShotNextAt = 0;
int gShotIndex = 0;
std::string gShotScreenBase;  // the last shot's file name without ".png"
bool gShotScreenDue[2] = {false, false};  // the screen's layers still to save for it: left / both eyes, right

// Saves the screen layer picture just drawn into the bound framebuffer, if
// one is due.
void maybeSaveScreenShot(int k, const vr::UiLayer& l, int w, int h) {
    int side = k == vr::kScreenRightLayer ? 1 : 0;
    if (!gShotScreenDue[side]) {
        return;
    }
    gShotScreenDue[side] = false;
    if (l.eye == vr::kBothEyes) gShotScreenDue[1] = false;
    std::shared_ptr<std::vector<unsigned char>> px(new std::vector<unsigned char>((size_t)w * h * 4));
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px->data());
    std::string file = gShotScreenBase + (l.eye == vr::kBothEyes ? "_screen.png" : side ? "_screenR.png" : "_screenL.png");
    PortHostAllocScope hostAlloc;
    std::thread([px, w, h, file] {
        PortHostAllocScope scope;
        for (size_t i = 3; i < px->size(); i += 4) (*px)[i] = 255;
        port_headless_write_png(file.c_str(), px->data(), w, h);
    }).detach();
}

void maybeSaveShot(App& a, int s, const uint32_t idx[2], GLuint motionTex = 0) {
    if (gShotIntervalNs <= 0 || port_host_time_ns() < gShotNextAt) {
        return;
    }
    gShotNextAt = port_host_time_ns() + gShotIntervalNs;
    int w = a.setRect[s][0].width, h = a.setRect[s][0].height;  // the part rendered
    std::shared_ptr<std::vector<unsigned char>> px(new std::vector<unsigned char>((size_t)w * h * 8));
    for (int e = 0; e < 2; e++) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, a.eyes[s][e].fbos[idx[e]]);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px->data() + (size_t)e * w * h * 4);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    char path[512];
    snprintf(path, sizeof(path), "%s/shot_%03d_f%llu.png", gShotDir.c_str(), gShotIndex++, (unsigned long long)gpu::renderer().frameNumber());
    std::string file = path;
    gShotScreenBase = file.substr(0, file.size() - 4);
    gShotScreenDue[0] = gShotScreenDue[1] = true;
    PortHostAllocScope hostAlloc;
    if (motionTex) {
        std::shared_ptr<std::vector<unsigned char>> mv(new std::vector<unsigned char>());
        std::string summary;
        vr::motionDebugImage(motionTex, mv.get(), &summary);
        std::string mvFile = file.substr(0, file.size() - 4) + "_mv.png";
        port_log("vr: %s: %s", mvFile.c_str(), summary.c_str());
        int mw = (int)a.motionW, mh = (int)a.motionH;
        std::thread([mv, mw, mh, mvFile] {
            PortHostAllocScope scope;
            port_headless_write_png(mvFile.c_str(), mv->data(), mw, mh);
        }).detach();
    }
    std::thread([px, w, h, file] {
        PortHostAllocScope scope;
        // Half size, left eye then right eye, rows bottom up as the PNG
        // writer takes them.
        int hw = w / 2, hh = h / 2;
        std::vector<unsigned char> out((size_t)hw * 2 * hh * 4);
        for (int e = 0; e < 2; e++) {
            const unsigned char* src = px->data() + (size_t)e * w * h * 4;
            for (int y = 0; y < hh; y++) {
                const unsigned char* r0 = src + (size_t)(2 * y) * w * 4;
                const unsigned char* r1 = src + (size_t)(2 * y + 1) * w * 4;
                unsigned char* d = out.data() + ((size_t)y * hw * 2 + (size_t)e * hw) * 4;
                for (int x = 0; x < hw; x++) {
                    for (int c = 0; c < 3; c++) {
                        d[x * 4 + c] = (unsigned char)((r0[x * 8 + c] + r0[x * 8 + 4 + c] + r1[x * 8 + c] + r1[x * 8 + 4 + c] + 2) / 4);
                    }
                    d[x * 4 + 3] = 255;
                }
            }
        }
        port_headless_write_png(file.c_str(), out.data(), hw * 2, hh);
    }).detach();
}

uint32_t acquireImage(Swapchain& sc) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XR_CHECK(xrAcquireSwapchainImage(sc.handle, &ai, &idx));
    XrSwapchainImageWaitInfo swi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    swi.timeout = XR_INFINITE_DURATION;
    XR_CHECK(xrWaitSwapchainImage(sc.handle, &swi));
    return idx;
}

void releaseImage(Swapchain& sc) {
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_CHECK(xrReleaseSwapchainImage(sc.handle, &ri));
}

// Gives up a half-rendered game frame (the display left 120 Hz, or the
// session stopped rendering): its set is never submitted.
void dropHeldEye(App& a) {
    if (a.heldSet >= 0) {
        releaseImage(a.eyes[a.heldSet][0]);
        a.heldSet = -1;
    }
    a.renderDue = false;
}

// Renders both eyes of the current game frame into swapchain set `s`.  The
// two images are released together, so whenever the compositor looks at
// the set, both eyes show the same game frame.
void renderPair(App& a, int s, const vr::FrameInfo& frame, const XrView views[2]) {
    uint32_t idx[2] = {0, 0};
    for (int e = 0; e < 2; e++) {
        Swapchain& sc = a.eyes[s][e];
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XR_CHECK(xrAcquireSwapchainImage(sc.handle, &ai, &idx[e]));
        XrSwapchainImageWaitInfo swi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        swi.timeout = XR_INFINITE_DURATION;
        XR_CHECK(xrWaitSwapchainImage(sc.handle, &swi));
    }
    for (int e = 0; e < 2; e++) {
        Swapchain& sc = a.eyes[s][e];
        vr::Extent used = vr::renderEye(e, frame, sc.fbos[idx[e]], sc.width, sc.height);
        a.setRect[s][e] = {used.width, used.height};
        a.setPose[s][e] = views[e].pose;
        a.setFov[s][e] = views[e].fov;
    }
    maybeSaveShot(a, s, idx);
    for (int e = 0; e < 2; e++) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        XR_CHECK(xrReleaseSwapchainImage(a.eyes[s][e].handle, &ri));
    }
}

// Frame pacing.  The game runs at 60 Hz.  At the default 120 Hz display
// refresh ("paired" refreshes), every game frame stays on screen for exactly
// two refreshes.  Refreshes alternate between two steps: the first picks up
// the newest game frame, moves the rig, signals the game's next retrace and
// renders the left eye into the free set of swapchains, keeping that image
// acquired; the second renders the right eye and releases both images
// together.  The set is submitted from the refresh after that, for two
// refreshes, while the other set is rendered.  So each refresh carries one
// eye of GPU work (the runtime holds the frame loop back when a refresh's
// work takes longer than the refresh), and the compositor only ever gets a
// set whose two eyes are finished and show the same game frame (submitting
// eyes as they were rendered, a late eye made it show the two eyes of
// different game frames).  The compositor's timewarp keeps head rotation
// current in between.
//
// With SpaceWarp (the space_warp setting, at 120 Hz) the runtime runs the
// frame loop at half the refresh rate instead: each frame picks up the
// newest game frame, signals the game's next retrace, renders both eyes and
// their motion vectors and depth, and goes out at once; the compositor
// synthesizes the refresh in between from the motion vectors (see
// vr::renderMotion), so things moving across the view no longer show twice
// in the same place.  Should the loop run at the full rate (as it may
// before the runtime settles), a game frame starts only every second
// refresh.
//
// At other refresh rates (72 Hz after thermal throttling, or a different
// refresh_rate setting) both eyes are rendered every refresh from the
// newest game frame, and the game keeps its own 59.94 Hz clock: frames then
// alternate between one and two refreshes, which makes moving things judder.
void renderFrame(App& a) {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XR_CHECK(xrWaitFrame(a.session, &wi, &fs));
    a.workStartNs = port_host_time_ns();
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    XR_CHECK(xrBeginFrame(a.session, &bi));
    updateAppSpace(a, fs.predictedDisplayTime);
    updatePassthrough(a);
    // The refresh_rate setting changed on the settings panel.
    if (a.requestedHz != 0.0f && vr::refreshRate() != a.requestedHz) {
        requestRefreshRate(a);
    }

    XrDuration period = fs.predictedDisplayPeriod;
    bool warp = a.hasSpaceWarp && vr::spaceWarp() && fabsf(a.displayHz - 120.0f) < 5.0f;
    if (warp != a.warp) {
        port_log("vr: SpaceWarp %s", warp ? "on: 60 frames a second with motion vectors, the compositor synthesizes the refreshes in between"
                                          : "off");
        a.warp = warp;
        a.warpFrames = 0;
        a.warpLastTime = 0;
        a.lastRetraceTime = 0;
    }
    // The display's refresh (the runtime's frame period may be the frame
    // loop's instead while SpaceWarp halves it).
    XrDuration refresh = warp ? (XrDuration)(1e9 / a.displayHz) : period;
    bool paired = !warp && period > 7900000 && period < 8800000;  // 120 Hz
    if (paired != a.paired) {
        port_log("vr: display period %.2f ms: %s", period / 1e6,
                 paired ? "each game frame spans two refreshes" : warp ? "SpaceWarp" : "game frames follow the display loosely");
        a.paired = paired;
    }
    if (a.lastDisplayTime != 0 && refresh > 0) {
        int64_t steps = (int64_t)llround((double)(fs.predictedDisplayTime - a.lastDisplayTime) / (double)refresh);
        // SpaceWarp frames normally come every two refreshes.
        int64_t missed = steps - (warp ? 2 : 1);
        if (missed > 0) {
            a.skippedRefreshes += (int)missed;
            // The runtime skips a refresh when a frame is handed in late
            // (the loop's own CPU work ran long: a texture upload, the
            // driver, the thread waiting for a core) or when a refresh's GPU
            // work overran it.  Only the second is a reason to render at a
            // lower resolution: GPU time per eye is often a third of the
            // refresh when the CPU is the one that is late.  Nor do refreshes
            // missed while the session lacks focus count (paused game,
            // system menu).
            if (a.lastWorkNs > (warp ? refresh * 2 : refresh) * 3 / 4) {
                a.lateRefreshes += (int)missed;
            } else if (a.focused) {
                vr::noteMissedRefreshes((int)missed);
            }
        }
    }
    a.lastDisplayTime = fs.predictedDisplayTime;
    int64_t now = port_host_time_ns();
    if (a.skippedRefreshes > 0 && now - a.skipLogAt > 10000000000ll) {
        port_log("vr: the frame loop missed %d refreshes in the last 10 s (%d with its own work late)", a.skippedRefreshes, a.lateRefreshes);
        a.skippedRefreshes = 0;
        a.lateRefreshes = 0;
        a.skipLogAt = now;
    }

    bool warpFrame = false;  // this frame goes out with motion vectors
    if (!fs.shouldRender) {
        dropHeldEye(a);
        updateInput(a, fs.predictedDisplayTime);
    } else if (warp) {
        dropHeldEye(a);
        a.renderedSet = -1;
        XrDuration interval = a.warpLastTime ? fs.predictedDisplayTime - a.warpLastTime : 2 * refresh;
        if (a.warpFrames < 12) {
            port_log("vr: SpaceWarp frame %d: display time +%.2f ms, predicted period %.2f ms", a.warpFrames, interval / 1e6, period / 1e6);
        }
        a.warpFrames++;
        a.warpLastTime = fs.predictedDisplayTime;
        updateInput(a, fs.predictedDisplayTime);
        XrView views[2];
        locateViews(a, fs.predictedDisplayTime, views);
        vr::FrameInfo frame = frameInfo(a, views, fs.predictedDisplayTime);
        // Both eyes share the frame's time: two refreshes, normally.
        interval = interval < refresh ? refresh : interval > 2 * refresh ? 2 * refresh : interval;
        frame.eyeBudgetMs = interval / 2e6f;
        frame.motion = true;
        vr::beginFrame(frame);
        if (a.lastRetraceTime == 0 || fs.predictedDisplayTime - a.lastRetraceTime >= refresh * 3 / 2) {
            if (gBooted) port_vi_retrace();  // the game starts its next frame now
            a.lastRetraceTime = fs.predictedDisplayTime;
        }
        // Always the same set: the compositor keeps SpaceWarp's state per
        // eye swapchain, and alternating sets made it start over every
        // frame, so it never synthesized one (VrApi "ASW=0").  Each frame is
        // rendered and submitted at once, so one set is enough.
        const int s = 0;
        uint32_t idx[2];
        for (int e = 0; e < 2; e++) {
            idx[e] = acquireImage(a.eyes[s][e]);
        }
        for (int e = 0; e < 2; e++) {
            Swapchain& sc = a.eyes[s][e];
            vr::Extent used = vr::renderEye(e, frame, sc.fbos[idx[e]], sc.width, sc.height);
            a.setRect[s][e] = {used.width, used.height};
            a.setPose[s][e] = views[e].pose;
            a.setFov[s][e] = views[e].fov;
        }
        uint32_t motionIdx[2];
        for (int e = 0; e < 2; e++) {
            motionIdx[e] = acquireImage(a.motion[e]);
            uint32_t depthIdx = acquireImage(a.depth[e]);
            vr::renderMotion(e, frame, a.motion[e].images[motionIdx[e]].image, a.depth[e].images[depthIdx].image);
        }
        xm::Quat dq;
        xm::Vec3 dp;
        a.warpSkip = vr::finishMotion(&dq, &dp);
        a.warpDelta.orientation = {dq.x, dq.y, dq.z, dq.w};
        a.warpDelta.position = {dp.x, dp.y, dp.z};
        maybeSaveShot(a, s, idx, a.motion[0].images[motionIdx[0]].image);
        for (int e = 0; e < 2; e++) {
            releaseImage(a.eyes[s][e]);
            releaseImage(a.motion[e]);
            releaseImage(a.depth[e]);
        }
        a.shownSet = s;
        a.renderSet = s ^ 1;  // for the paired refreshes, should SpaceWarp stop
        warpFrame = true;
    } else if (paired && !a.renderDue) {
        // First step.  The set finished on the last refresh goes up now.
        if (a.renderedSet >= 0) {
            a.shownSet = a.renderedSet;
            a.renderedSet = -1;
        }
        // The newest game frame is finished on the next refresh and shown on
        // the two after it: rendered for the time between those.
        a.setTime = fs.predictedDisplayTime + period * 5 / 2;
        updateInput(a, a.setTime);
        XrView views[2];
        locateViews(a, a.setTime, views);
        a.setFrame = frameInfo(a, views, a.setTime);
        a.setFrame.eyeBudgetMs = period / 1e6f;  // one eye per refresh
        vr::beginFrame(a.setFrame);
        if (gBooted) port_vi_retrace();  // the game starts its next frame now
        Swapchain& sc = a.eyes[a.renderSet][0];
        a.heldIdx = acquireImage(sc);
        vr::Extent used = vr::renderEye(0, a.setFrame, sc.fbos[a.heldIdx], sc.width, sc.height);
        a.setRect[a.renderSet][0] = {used.width, used.height};
        a.setPose[a.renderSet][0] = views[0].pose;
        a.setFov[a.renderSet][0] = views[0].fov;
        a.heldSet = a.renderSet;
        a.renderDue = true;
    } else if (paired && a.heldSet >= 0) {
        // Second step: the right eye, for the same display time with the
        // freshest prediction of it; then both images go out together.
        XrView views[2];
        locateViews(a, a.setTime, views);
        vr::FrameInfo frame = a.setFrame;
        frame.eyes[1] = eyeInfo(a, views[1], 1);
        int s = a.heldSet;
        Swapchain& sc = a.eyes[s][1];
        uint32_t idx[2] = {a.heldIdx, acquireImage(sc)};
        vr::Extent used = vr::renderEye(1, frame, sc.fbos[idx[1]], sc.width, sc.height);
        a.setRect[s][1] = {used.width, used.height};
        a.setPose[s][1] = views[1].pose;
        a.setFov[s][1] = views[1].fov;
        maybeSaveShot(a, s, idx);
        releaseImage(a.eyes[s][0]);
        releaseImage(sc);
        a.heldSet = -1;
        a.renderedSet = s;
        a.renderSet = s ^ 1;
        a.renderDue = false;
    } else {
        dropHeldEye(a);
        if (a.renderedSet >= 0) {
            a.shownSet = a.renderedSet;
            a.renderedSet = -1;
        }
        updateInput(a, fs.predictedDisplayTime);
        XrView views[2];
        locateViews(a, fs.predictedDisplayTime, views);
        vr::FrameInfo frame = frameInfo(a, views, fs.predictedDisplayTime);
        frame.eyeBudgetMs = period / 2e6f;  // both eyes in one refresh
        vr::beginFrame(frame);
        renderPair(a, a.renderSet, frame, views);
        a.shownSet = a.renderSet;
        a.renderSet ^= 1;
    }

    XrCompositionLayerProjectionView projViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerSpaceWarpInfoFB warpInfo[2] = {{XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB}, {XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB}};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    // The room first (while its passthrough runs), under everything; then
    // the eye layer, see-through around the giant screen where the room is
    // to show; then the panels.
    XrCompositionLayerPassthroughFB room{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    const XrCompositionLayerBaseHeader* layers[2 + vr::kUiLayerCount];
    uint32_t baseCount = 0;
    if (fs.shouldRender && a.passthroughRunning) {
        room.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        room.space = XR_NULL_HANDLE;
        room.layerHandle = a.passthroughLayer;
        layers[baseCount++] = (const XrCompositionLayerBaseHeader*)&room;
        layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied
    }
    layers[baseCount] = (const XrCompositionLayerBaseHeader*)&layer;
    uint32_t layerCount = 0;
    if (fs.shouldRender && a.shownSet >= 0) {
        // The set on show, with the poses its eyes were rendered for (the
        // compositor reprojects them to the display pose).
        for (int e = 0; e < 2; e++) {
            const Swapchain& sc = a.eyes[a.shownSet][e];
            projViews[e].pose = a.setPose[a.shownSet][e];
            projViews[e].fov = a.setFov[a.shownSet][e];
            projViews[e].subImage.swapchain = sc.handle;
            projViews[e].subImage.imageRect.offset = {0, 0};
            projViews[e].subImage.imageRect.extent = a.setRect[a.shownSet][e];  // the part rendered
            if (warpFrame) {
                XrCompositionLayerSpaceWarpInfoFB& w = warpInfo[e];
                w.layerFlags = a.warpSkip ? XR_COMPOSITION_LAYER_SPACE_WARP_INFO_FRAME_SKIP_BIT_FB : 0;
                w.motionVectorSubImage.swapchain = a.motion[e].handle;
                w.motionVectorSubImage.imageRect = {{0, 0}, {a.motion[e].width, a.motion[e].height}};
                w.appSpaceDeltaPose = a.warpDelta;
                w.depthSubImage.swapchain = a.depth[e].handle;
                w.depthSubImage.imageRect = {{0, 0}, {a.depth[e].width, a.depth[e].height}};
                w.minDepth = 0.0f;
                w.maxDepth = 1.0f;
                w.nearZ = vr::kNearZ;
                w.farZ = vr::kFarZ;
                projViews[e].next = &w;
            }
        }
        layer.space = a.appSpace;
        layer.viewCount = 2;
        layer.views = projViews;
        layerCount = 1;
    }
    // The panels with text, over the eye layer (their images are drawn when
    // they changed; a layer shows the image last released).
    XrCompositionLayerQuad quads[vr::kUiLayerCount];
    XrCompositionLayerSettingsFB quadSettings[vr::kUiLayerCount];
    uint32_t quadCount = 0;
    if (layerCount && vr::uiLayers()) {
        for (int k : vr::kUiLayerOrder) {
            vr::UiLayer l = vr::uiLayer(k);
            if (!l.visible) continue;
            Swapchain& sc = a.ui[k];
            if (l.changed || !a.uiHasImage[k]) {
                uint32_t idx = acquireImage(sc);
                vr::drawUiLayer(k, sc.fbos[idx]);
                if (k == vr::kScreenLayer || k == vr::kScreenRightLayer) {
                    glBindFramebuffer(GL_FRAMEBUFFER, sc.fbos[idx]);
                    maybeSaveScreenShot(k, l, l.imageWidth ? l.imageWidth : sc.width, l.imageHeight ? l.imageHeight : sc.height);
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                }
                if (sc.mips > 1) {
                    glBindTexture(GL_TEXTURE_2D, sc.images[idx].image);
                    glGenerateMipmap(GL_TEXTURE_2D);
                    glBindTexture(GL_TEXTURE_2D, 0);
                }
                releaseImage(sc);
                a.uiHasImage[k] = true;
            }
            XrCompositionLayerQuad& q = quads[quadCount++];
            q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied
            q.space = a.appSpace;
            q.eyeVisibility = l.eye == vr::kLeftEye ? XR_EYE_VISIBILITY_LEFT : l.eye == vr::kRightEye ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = sc.handle;
            q.subImage.imageRect = {{0, 0}, {l.imageWidth ? l.imageWidth : sc.width, l.imageHeight ? l.imageHeight : sc.height}};
            q.pose.orientation = {l.orientation.x, l.orientation.y, l.orientation.z, l.orientation.w};
            q.pose.position = {l.position.x, l.position.y, l.position.z};
            q.size = {l.width, l.height};
            if (a.hasLayerSettings && (k == vr::kScreenLayer || k == vr::kScreenRightLayer)) {
                // Meta's supersampling filter against the flicker of a layer
                // with more texels than the display has pixels for it.
                quadSettings[k] = {XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
                quadSettings[k].layerFlags = XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB;
                q.next = &quadSettings[k];
            }
            layers[baseCount + layerCount + quadCount - 1] = (const XrCompositionLayerBaseHeader*)&q;
        }
    }
    // Meta Quest Super Resolution (the compositor's upscaling and
    // sharpening filter) on the eye layer.
    // Only while it holds the diorama: around the screen it is dark or
    // see-through, and the filter would cost the compositor its time for
    // nothing.
    XrCompositionLayerSettingsFB layerSettings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
    if (layerCount && a.hasLayerSettings && vr::superResolution() && port_vr_diorama()) {
        layerSettings.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;
        layer.next = &layerSettings;
    }
    // Meta's dynamic resolution: what the runtime recommends for this layer
    // (logged every 10 s; asking also tells the runtime the app scales its
    // resolution, which is what opens GPU level 5 on Quest 3).
    if (layerCount && a.xrGetRecommendedLayerResolutionMETA) {
        XrRecommendedLayerResolutionGetInfoMETA ri{XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_GET_INFO_META};
        ri.layer = (const XrCompositionLayerBaseHeader*)&layer;
        ri.predictedDisplayTime = fs.predictedDisplayTime;
        XrRecommendedLayerResolutionMETA rr{XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_META};
        if (XR_SUCCEEDED(a.xrGetRecommendedLayerResolutionMETA(a.session, &ri, &rr)) && rr.isValid) {
            int w = rr.recommendedImageDimensions.width;
            a.recMinW = a.recValid ? (w < a.recMinW ? w : a.recMinW) : w;
            a.recMaxW = a.recValid ? (w > a.recMaxW ? w : a.recMaxW) : w;
            a.recValid++;
        }
        a.recSamples++;
        int64_t t = port_host_time_ns();
        if (t - a.recLogAt > 10000000000ll) {
            if (a.recLogAt) {
                float base = (float)a.viewConfig[0].recommendedImageRectWidth;
                port_log("vr: runtime recommends eye widths %d-%d (scale %.2f-%.2f; %d of %d frames valid)", a.recMinW, a.recMaxW, a.recMinW / base,
                         a.recMaxW / base, a.recValid, a.recSamples);
            }
            a.recLogAt = t;
            a.recValid = a.recSamples = 0;
        }
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    // Without the eye layer (nothing rendered yet) nothing goes out, the room
    // included.
    ei.layerCount = layerCount ? baseCount + layerCount + quadCount : 0;
    ei.layers = layers;
    XrResult end = xrEndFrame(a.session, &ei);
    if (XR_FAILED(end) && warpFrame) {
        // The runtime refused the motion vectors: go on without SpaceWarp.
        port_log("vr: xrEndFrame with SpaceWarp failed (%d): SpaceWarp off", (int)end);
        a.hasSpaceWarp = false;
    } else {
        XR_CHECK(end);
    }
    a.lastWorkNs = port_host_time_ns() - a.workStartNs;
    // The runtime's measure of the GPU (for the dynamic resolution): an app
    // frame spans two eyes and two refreshes with SpaceWarp, one eye and one
    // refresh when the eyes take alternate refreshes, both eyes and one
    // refresh otherwise.
    if (a.hasPerfMetrics && fs.shouldRender) {
        static bool sUnitsLogged = false;
        XrPerformanceMetricsCounterUnitMETA units[3] = {};
        float app = queryCounter(a, a.appGpuPath, &units[0]);
        float compositor = queryCounter(a, a.compositorGpuPath, &units[1]);
        float util = queryCounter(a, a.gpuUtilPath, &units[2]);
        if (!sUnitsLogged && app > 0.0f) {
            sUnitsLogged = true;
            port_log("XR performance metrics: app GPU %.2f (unit %d), compositor GPU %.2f (unit %d), GPU utilization %.1f (unit %d)", app, (int)units[0],
                     compositor, (int)units[1], util, (int)units[2]);
        }
        int eyes = warpFrame ? 2 : a.paired ? 1 : 2;
        int refreshes = warpFrame ? 2 : 1;
        vr::notePerformance(app, compositor, util, eyes, refreshes, refresh / 1e6f);
    }
}

// Android's all files access (MANAGE_EXTERNAL_STORAGE), through JNI (the app
// has no Java code): whether the app has it, and opening the settings page
// where the player grants it.
JNIEnv* jniEnv(App& a) {
    JNIEnv* env = nullptr;
    if (a.android->activity->vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        a.android->activity->vm->AttachCurrentThread(&env, nullptr);
    }
    return env;
}

bool hasAllFilesAccess(App& a) {
    JNIEnv* env = jniEnv(a);
    if (!env) return false;
    jclass cls = env->FindClass("android/os/Environment");
    jmethodID m = cls ? env->GetStaticMethodID(cls, "isExternalStorageManager", "()Z") : nullptr;
    bool granted = m && env->CallStaticBooleanMethod(cls, m);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        granted = false;
    }
    if (cls) env->DeleteLocalRef(cls);
    return granted;
}

void requestAllFilesAccess(App& a) {
    JNIEnv* env = jniEnv(a);
    if (!env) return;
    jobject activity = a.android->activity->clazz;
    jclass actCls = env->GetObjectClass(activity);
    jmethodID getPackageName = env->GetMethodID(actCls, "getPackageName", "()Ljava/lang/String;");
    jstring pkg = (jstring)env->CallObjectMethod(activity, getPackageName);
    const char* pkgName = pkg ? env->GetStringUTFChars(pkg, nullptr) : nullptr;
    std::string uriText = std::string("package:") + (pkgName ? pkgName : "com.galaxy.quest");
    if (pkgName) env->ReleaseStringUTFChars(pkg, pkgName);
    jclass uriCls = env->FindClass("android/net/Uri");
    jmethodID parse = env->GetStaticMethodID(uriCls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jstring uriStr = env->NewStringUTF(uriText.c_str());
    jobject uri = env->CallStaticObjectMethod(uriCls, parse, uriStr);
    jclass intentCls = env->FindClass("android/content/Intent");
    jmethodID withUri = env->GetMethodID(intentCls, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
    jmethodID plain = env->GetMethodID(intentCls, "<init>", "(Ljava/lang/String;)V");
    jmethodID start = env->GetMethodID(actCls, "startActivity", "(Landroid/content/Intent;)V");
    // This app's own page first, then the list of all apps with the setting.
    jstring action = env->NewStringUTF("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION");
    jobject intent = env->NewObject(intentCls, withUri, action, uri);
    env->CallVoidMethod(activity, start, intent);
    bool ok = !env->ExceptionCheck();
    if (!ok) {
        env->ExceptionClear();
        jstring listAction = env->NewStringUTF("android.settings.MANAGE_ALL_FILES_ACCESS_PERMISSION");
        jobject listIntent = env->NewObject(intentCls, plain, listAction);
        env->CallVoidMethod(activity, start, listIntent);
        ok = !env->ExceptionCheck();
        if (!ok) env->ExceptionClear();
    }
    port_log("setup: all files access settings %s", ok ? "opened" : "could not be opened");
}

void bootGame(const std::string& dataRoot) {
    port_log("game files: %s", dataRoot.c_str());
    gBooted = true;
    port_boot(dataRoot.c_str(), gSaveRoot.c_str());
}

void onAppCmd(android_app* app, int32_t cmd) {
    (void)app;
    switch (cmd) {
    case APP_CMD_RESUME:
        port_log("APP_CMD_RESUME");
        break;
    case APP_CMD_PAUSE:
        port_log("APP_CMD_PAUSE");
        break;
    case APP_CMD_DESTROY:
        port_log("APP_CMD_DESTROY");
        break;
    default:
        break;
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void port_android_main(android_app* app, uintptr_t windowBase, size_t windowSize) {
    PortHostAllocScope scope;
    App& a = gApp;
    a.android = app;
    app->onAppCmd = onAppCmd;
    port_mem_set_reserved_window(windowBase, windowSize);

    // Cooked game data is pushed to the app's external files dir; saves go to
    // internal storage.
    std::string ext = app->activity->externalDataPath ? app->activity->externalDataPath : "/sdcard/Android/data/com.galaxy.quest/files";
    std::string saveRoot = std::string(app->activity->internalDataPath) + "/nand";
    mkdir(saveRoot.c_str(), 0770);

    port_log_file((ext + "/petari_log.txt").c_str());
    loadDebugEnv(ext + "/petari_debug.env");
    if (const char* script = getenv("PETARI_INPUT")) {
        gScriptCount = portParseInputScript(script, gScript, 1024);
    }
    if (const char* saves = getenv("PETARI_SAVE_DIR")) {
        saveRoot = saves;  // tests keep away from the player's saves
    }
    gSaveRoot = saveRoot;
    if (const char* shots = getenv("PETARI_VRSHOT_MS")) {
        gShotIntervalNs = (int64_t)atoi(shots) * 1000000;
        gShotDir = ext + "/vrshots";
        mkdir(gShotDir.c_str(), 0770);
    }
    vr::loadSettings((ext + "/petari_vr.ini").c_str());  // before the swapchains are sized
    initEgl(a);
    initInstance(a);
    initActions(a);
    initSession(a);
    gpu::setShaderCachePath((std::string(app->activity->internalDataPath) + "/shaders.bin").c_str());
    vr::init();
    initUiLayers(a);
    initPerfMetrics(a);

    // The boot time, for debug hooks timed from it (PETARI_INPUT, PETARI_WARP).
    gBootNs = port_host_time_ns();
    char bootMs[32];
    snprintf(bootMs, sizeof(bootMs), "%lld", (long long)(gBootNs / 1000000));
    setenv("PETARI_T0_MS", bootMs, 1);
    // The game's files: the folder chosen on the setup screen before, else
    // the app's own files/game.  Without them the setup screen opens (the
    // game would stop at its first file).
    std::string dataRoot;
    bool ready = false;
    if (const char* only = getenv("PETARI_GAME_ROOT")) {
        // Debug (petari_debug.env): look only there, e.g. a missing folder
        // to see the setup screen without moving the player's files.
        if (vr::isGameFolder(only, &ready) && ready) dataRoot = only;
    } else if (!vr::gamePath().empty() && vr::isGameFolder(vr::gamePath(), &ready) && ready) {
        dataRoot = vr::gamePath();
    } else if (vr::isGameFolder(ext + "/game", &ready) && ready) {
        dataRoot = ext + "/game";
    }
    if (!dataRoot.empty()) {
        bootGame(dataRoot);
    } else {
        vr::setupSetStorageAccess(hasAllFilesAccess(a));
        vr::setupStart(ext, vr::gamePath().empty() ? ext + "/game" : vr::gamePath());
    }

    bool quit = false;
    while (!quit && !app->destroyRequested) {
        int events;
        android_poll_source* source;
        int timeout = a.sessionRunning ? 0 : 50;
        while (ALooper_pollOnce(timeout, nullptr, &events, (void**)&source) >= 0) {
            if (source) {
                source->process(app, source);
            }
            if (app->destroyRequested) {
                break;
            }
            timeout = 0;
        }
        handleEvents(a, quit);
        if (!gBooted) {
            std::string chosen;
            if (vr::setupTakeChoice(&chosen)) {
                vr::saveGamePath(chosen);
                bootGame(chosen);
            }
            if (vr::setupTakeAccessRequest()) {
                requestAllFilesAccess(a);
            }
            // The player may grant access in Android's settings and come back.
            int64_t now = port_host_time_ns();
            if (now - gAccessCheckAt > 1000000000) {
                gAccessCheckAt = now;
                vr::setupSetStorageAccess(hasAllFilesAccess(a));
            }
        }
        if (a.sessionRunning) {
            renderFrame(a);
        }
    }
    port_log("exiting");
    exit(0);
}
