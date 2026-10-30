#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

using f32 = float;
using s32 = int;
constexpr f32 PI_180 = 3.14159265358979323846f / 180.0f;
struct TVec2f {
    f32 x{}, y{};
    template<class T> void set(T a, T b) { x = a; y = b; }
    f32 squared(const TVec2f& b) const { return (x-b.x)*(x-b.x) + (y-b.y)*(y-b.y); }
};
struct TVec3f {
    f32 x{}, y{}, z{};
    TVec3f() = default;
    TVec3f(f32 a, f32 b, f32 c) : x(a), y(b), z(c) {}
    void scale(f32 n, const TVec3f& b) { x=n*b.x; y=n*b.y; z=n*b.z; }
    f32 squared() const { return x*x+y*y+z*z; }
};
struct TVec4f : TVec3f {
    f32 w;
    TVec4f(f32 a, f32 b, f32 c, f32 d) : TVec3f(a,b,c), w(d) {}
    TVec3f* toTVec3() { return this; }
};
namespace JGeometry { template<class T> struct TUtil { static T PI() { return 3.14159265358979323846f; } }; }
struct TProj3f {
    f32 mMtx[4][4]{};
    void setInline(const TProj3f& b) { *this = b; }
#include "dome_zoom_projection_actual.inc"
};
struct SceneController {};
struct GameSystem { SceneController* mSceneController; };
static SceneController controller;
static GameSystem systemState{&controller};
template<class T> struct SingletonHolder { static T* get() { return &systemState; } };
constexpr int SceneObj_DemoDirector = 1;
static bool game = true, fileSelect = false, demo = false, eventCamera = false, dome = true;
static f32 requestedZoom = 1.5f, currentFovy = 45.0f, screenWidth = 832.0f;
static TProj3f projection;
static int guardReads;
static const char* observedGetenv(const char* name) {
    if (!std::strcmp(name, "GXV_ASTRO_DOME_ZOOM_GUARD")) ++guardReads;
    return std::getenv(name);
}
extern "C" f32 port_camera_zoom() { return requestedZoom; }
extern "C" void port_log(const char*, ...) {}
int port_vr_pointer_ray(f32*, f32*) { return 0; }
namespace MR {
bool isEqualSceneName(const char*) { return game; }
bool isStageFileSelect() { return fileSelect; }
bool isEqualStageName(const char* name) { return dome && !std::strcmp(name,"AstroDome"); }
bool isExistSceneObj(int) { return true; }
bool isDemoActive() { return demo; }
bool isEventCameraActive() { return eventCamera; }
f32 getScreenWidth() { return screenWidth; }
f32 getScreenHeight() { return 456.0f; }
f32 getFovy() { return currentFovy; }
f32 tan(f32 x) { return std::tan(x); }
f32 abs(f32 x) { return std::fabs(x); }
const TProj3f& getStarPointerProjMtx() { return projection; }
int getStarPointerViewMtx() { return 0; }
}
void PSMTXMultVec(int, const TVec3f* a, TVec3f* b) { *b = *a; }
class StarPointerTarget {
public:
    f32 mRadius3d = 200.0f;
    TVec3f position;
    void calcPosition(TVec3f* out) const { *out = position; }
    bool isPointing(const TVec2f&, f32, f32) const;
    f32 calcRadius2d(f32, f32, f32) const;
    bool calcScreenPositionFromView(TVec2f*, const TVec3f&) const;
};
#define getenv observedGetenv
#include "dome_zoom_camera_actual.inc"
#undef getenv
#include "dome_zoom_target_actual.inc"

static bool close(f32 a, f32 b) { return std::fabs(a-b) < 0.0001f; }
static f32 originalZoom(f32 fovy, f32 zoom) { return 2*std::atan(std::tan(fovy*PI_180/2)/zoom)/PI_180; }
static void resetZoom() { sZoomNow=1; sZoomKnown=false; }
static void updateProjection(f32 fovy, f32 aspect) {
    currentFovy=fovy;
    projection.makePerspective(fovy, aspect, 100, 800000);
}

int main(int argc, char** argv) {
    assert(argc==2 || argc==6);
    const bool guarded=!std::strcmp(argv[1],"on");
    // A synthetic near-edge target. Optional view-space inputs allow a private
    // asset-derived reproduction without distributing game data in the test.
    TVec3f view(0, 0.7f*1500*std::tan(45*PI_180/2), -1500);
    f32 baseFovy=45;
    if (argc==6) {
        view=TVec3f(std::atof(argv[2]),std::atof(argv[3]),std::atof(argv[4]));
        baseFovy=std::atof(argv[5]);
    }
    StarPointerTarget target; target.position=view;
    int clipped=0;
    for (f32 aspect : {4.0f/3,16.0f/9}) {
        screenWidth=aspect<1.5f ? 608 : 832;
        for (int tenth=10;tenth<=20;++tenth) {
            requestedZoom=tenth/10.0f;
            resetZoom();
            const f32 fovy=zoomedFovy(baseFovy,true);
            assert(close(fovy,guarded ? baseFovy : originalZoom(baseFovy,requestedZoom)));
            updateProjection(fovy,aspect);
            TVec2f screen;
            const bool visible=target.calcScreenPositionFromView(&screen,view);
            if (!visible) ++clipped;
            if (guarded || tenth==10) {
                assert(visible);
                assert(target.isPointing(screen,99999,16));
            }
            std::printf("dome guard=%d aspect=%.4f zoom=%.1f fovy=%.6f center-visible=%d\n",guarded,aspect,requestedZoom,fovy,visible);
        }
    }
    assert(guarded ? clipped==0 : clipped>0);
    // Reading once also means an in-process environment edit cannot change it.
    setenv("GXV_ASTRO_DOME_ZOOM_GUARD",guarded ? "0" : "1",1);
    resetZoom(); requestedZoom=2;
    assert(close(zoomedFovy(45,true),guarded ? 45 : originalZoom(45,2)));
    assert(guardReads==1);
    // All other gameplay retains the setting, including transitions away from
    // a dome. File select, non-game scenes and event/demo relaxation stay intact.
    dome=false;
    for (int tenth=10;tenth<=20;++tenth) {
        requestedZoom=tenth/10.0f; resetZoom();
        assert(close(zoomedFovy(45,true),originalZoom(45,requestedZoom)));
    }
    requestedZoom=2; resetZoom();
    f32 fovy=zoomedFovy(45,true);
    assert(fovy<45);
    dome=true;
    fovy=zoomedFovy(45,false);
    assert(close(fovy,guarded ? 45 : originalZoom(45,2)));
    dome=false;
    const f32 before=sZoomNow;
    fovy=zoomedFovy(45,false);
    assert(guarded ? sZoomNow>before && sZoomNow<2 : close(sZoomNow,2));
    for (int i=0;i<1000;++i) fovy=zoomedFovy(45,false);
    assert(close(fovy,originalZoom(45,2)));
    for (int mode=0;mode<2;++mode) {
        demo=mode==0; eventCamera=mode==1;
        for (int i=0;i<1000;++i) fovy=zoomedFovy(45,false);
        assert(close(fovy,45));
    }
    demo=eventCamera=false;
    for (int mode=0;mode<3;++mode) {
        game=mode!=0; fileSelect=mode==1;
        systemState.mSceneController=mode==2 ? nullptr : &controller;
        resetZoom(); assert(close(zoomedFovy(45,true),45));
    }
    assert(zoomedFovy(0,false)==0 && zoomedFovy(170,false)==170);
    assert(guardReads==1);
    std::puts("PASS: dome zoom, target visibility, OFF/ON, read-once, other scenes and transitions");
}
