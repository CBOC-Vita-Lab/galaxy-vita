#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static std::string contents;
static bool handheld=true, fileExists=false;
static size_t at;
static int opens;
static FILE* fakeOpen(const char*, const char* mode) {
    assert(!std::strcmp(mode,"r"));
    ++opens; at=0;
    return fileExists ? reinterpret_cast<FILE*>(&at) : nullptr;
}
static char* fakeGets(char* out, int size, FILE* f) {
    assert(f==reinterpret_cast<FILE*>(&at));
    if (at==contents.size()) return nullptr;
    size_t n=0;
    while (n+1<static_cast<size_t>(size) && at<contents.size()) {
        char c=contents[at++]; out[n++]=c;
        if (c=='\n') break;
    }
    out[n]=0; return out;
}
static int fakeClose(FILE* f) { assert(f==reinterpret_cast<FILE*>(&at)); return 0; }
int port_handheld() { return handheld; }
void port_log_summary(const char*,...) {}
static int cameraInverted=-1;
static int externalPad=0;
extern "C" void port_input_set_camera_inverted(int v) { cameraInverted=v; }
extern "C" void port_input_set_camera_invert_mask(int v) { cameraInverted=v; }
extern "C" int port_vita_input_external_pad(void) { return externalPad; }
#define fopen fakeOpen
#define fgets fakeGets
#define fclose fakeClose
#include "dome_zoom_settings_actual.inc"
#undef fopen
#undef fgets
#undef fclose

int main() {
    unsetenv("PETARI_ZOOM");
    assert(port_camera_zoom()==1.5f);
    handheld=false; sZoomTenths=-1;
    assert(port_camera_zoom()==1.0f);
    handheld=true;
    for (int tenth=10;tenth<=20;++tenth) {
        const std::string value=std::to_string(tenth/10.0);
        sZoomTenths=-1; setenv("PETARI_ZOOM",value.c_str(),1);
        assert(std::fabs(port_camera_zoom()-tenth/10.0f)<1e-6);
    }
    sZoomFile="memory-only"; fileExists=true;
    for (int tenth=10;tenth<=20;++tenth) {
        contents="ignored = 7\n zoom = "+std::to_string(tenth/10.0)+"\nrear_aim = 1\n";
        setenv("PETARI_ZOOM","2.0",1); sZoomTenths=-1;
        assert(std::fabs(port_camera_zoom()-tenth/10.0f)<1e-6);
        const int before=opens; contents="zoom = 1.0\n";
        assert(std::fabs(port_camera_zoom()-tenth/10.0f)<1e-6 && opens==before);
    }
    contents="zoom = 0.5\n"; sZoomTenths=-1; assert(port_camera_zoom()==1);
    contents="zoom = 4.0\n"; sZoomTenths=-1; assert(port_camera_zoom()==2);
    contents="zoom = invalid\n"; sZoomTenths=-1; assert(port_camera_zoom()==2);
    contents="zoom = 1.0\n"; sZoomTenths=-1;
    for (int tenth=11;tenth<=20;++tenth) assert(port_zoom_adjust(1)==tenth);
    assert(port_zoom_adjust(1)==20 && sZoomLimitHits==1);
    for (int tenth=19;tenth>=10;--tenth) assert(port_zoom_adjust(-1)==tenth);
    assert(port_zoom_adjust(-1)==10 && sZoomLimitHits==2);
    // settingsnative: the keys the native Settings added (tap_jump, invert_camera), the live setters and the written text, round trip.
    {
        auto reset=[]{ sRearAim=-1; sStickPointer=-1; sFreeCamera=-1; sRightStickMode=-1; sRightStickEnv=-2; sCameraSpeed=-1; sCameraSpeedEnv=-2; sStarBitReach=-1; sTapJump=-1; sInvertCamera=-1; sZoomTenths=-1; };
        unsetenv("PETARI_ZOOM"); unsetenv("PETARI_STAR_BIT_REACH"); unsetenv("PETARI_ZOOM_FORCE"); unsetenv("GXV_FREE_CAM"); unsetenv("GXV_FREE_CAM_SPEED"); unsetenv("PETARI_RSTICK");
        sZoomFile="memory-only"; fileExists=true;
        contents="zoom = 1.5\n"; reset();
        assert(port_tap_jump_enabled()==1 && port_invert_camera_enabled()==0 && port_free_camera_enabled()==1 && port_right_stick_mode()==0 && port_camera_speed()==5);
        contents="tap_jump = 0\ninvert_camera = 3\nfree_camera = 0\ncamera_speed = 8\n"; reset();
        assert(port_tap_jump_enabled()==0 && port_invert_camera_enabled()==3 && port_free_camera_enabled()==0 && port_right_stick_mode()==1 && port_camera_speed()==8);
        contents="tap_jump = x\ninvert_camera = \nfree_camera = ?\ncamera_speed = 99\n"; reset();
        assert(port_tap_jump_enabled()==1 && port_invert_camera_enabled()==0 && port_free_camera_enabled()==1 && port_camera_speed()==5);   // damaged lines keep defaults
        contents="tap_jump = 1\ntap_jump = 0\n"; reset(); assert(port_tap_jump_enabled()==0);       // the last one counts, as for the other keys
        fileExists=false; reset(); assert(port_tap_jump_enabled()==1 && port_invert_camera_enabled()==0 && port_star_bit_reach()==1 && port_free_camera_enabled()==1 && port_camera_speed()==5);
        fileExists=true; contents="zoom = 1.5\n"; reset();
        // the setters: a change counts for the save (sZoomChange), the same value does not; Invert Camera reaches the input layer at once
        unsigned c=sZoomChange;
        assert(port_tap_jump_set(1)==1 && sZoomChange==c);
        assert(port_tap_jump_set(0)==0 && port_tap_jump_enabled()==0 && sZoomChange==c+1);
        assert(port_tap_jump_set(7)==1 && port_tap_jump_enabled()==1 && sZoomChange==c+2);
        cameraInverted=-1; assert(port_invert_camera_set(0)==0 && sZoomChange==c+2 && cameraInverted==0);   // (already off: no save, still applied)
        assert(port_invert_camera_set(1)==1 && port_invert_camera_enabled()==1 && cameraInverted==1 && sZoomChange==c+3);
        assert(port_invert_camera_set(2)==2 && port_invert_camera_enabled()==2 && cameraInverted==2 && sZoomChange==c+4);
        assert(port_invert_camera_set(3)==3 && port_invert_camera_enabled()==3 && cameraInverted==3 && sZoomChange==c+5);
        assert(port_invert_camera_set(1)==1 && sZoomChange==c+6);
        assert(port_rear_aim_set(1)==1 && port_rear_aim_set(1)==1 && sZoomChange==c+7);
        assert(port_stick_pointer_set(1)==1 && port_right_stick_mode()==2 && port_free_camera_enabled()==0 && sZoomChange==c+8);
        assert(port_right_stick_mode_set(1)==1 && port_stick_pointer_enabled()==0 && port_free_camera_enabled()==0 && sZoomChange==c+9);
        assert(port_right_stick_mode_set(0)==0 && port_free_camera_enabled()==1 && sZoomChange==c+10);
        assert(port_right_stick_mode_set(2)==2 && port_stick_pointer_enabled()==1 && sZoomChange==c+11);
        assert(port_camera_speed_set(7)==7 && port_camera_speed()==7 && port_camera_speed_set(7)==7 && sZoomChange==c+12);
        assert(port_camera_speed_set(99)==10 && port_camera_speed_set(0)==1 && port_camera_speed_set(5)==5 && sZoomChange==c+15);
        assert(port_star_bit_reach_set(4)==4 && port_star_bit_reach_set(99)==5 && port_star_bit_reach_set(-3)==1 && sZoomChange==c+18);
        // everything written comes back as it was
        port_zoom_adjust(3);   // 1.5 -> 1.8
        char text[768];
        const int len=ZoomFileText(text,sizeof text);
        assert(len>0 && len<static_cast<int>(sizeof text));
        const std::string written(text,static_cast<size_t>(len));
        assert(written.find("zoom = 1.8\n")!=std::string::npos && written.find("rear_aim = 1\n")!=std::string::npos && written.find("stick_pointer = 1\n")!=std::string::npos &&
               written.find("free_camera = 0\n")!=std::string::npos && written.find("camera_speed = 5\n")!=std::string::npos &&
               written.find("star_bit_reach = 1\n")!=std::string::npos && written.find("tap_jump = 1\n")!=std::string::npos && written.find("invert_camera = 1\n")!=std::string::npos);
        // Verify PETARI_RSTICK & GXV_FREE_CAM single-source-of-truth cached env overrides
        // AND verify temporary env overrides NEVER clobber settings.ini in ZoomFileText()!
        setenv("PETARI_RSTICK", "free", 1); sRightStickEnv=-2; assert(port_right_stick_mode()==0 && port_free_camera_enabled()==1 && port_stick_pointer_enabled()==0);
        setenv("PETARI_RSTICK", "classic", 1); sRightStickEnv=-2; assert(port_right_stick_mode()==1 && port_free_camera_enabled()==0 && port_stick_pointer_enabled()==0);
        setenv("PETARI_RSTICK", "pointer", 1); sRightStickEnv=-2; assert(port_right_stick_mode()==2 && port_free_camera_enabled()==0 && port_stick_pointer_enabled()==1);
        setenv("GXV_FREE_CAM_SPEED", "9", 1); sCameraSpeedEnv=-2; assert(port_camera_speed()==9);
        // File still holds mode=2 (Pointer) & speed=5; env override (free + speed 9) must NOT overwrite ZoomFileText!
        setenv("PETARI_RSTICK", "free", 1); sRightStickEnv=-2;
        assert(port_right_stick_mode()==0 && port_camera_speed()==9);
        {
            const int envLen = ZoomFileText(text, sizeof text);
            const std::string envWritten(text, static_cast<size_t>(envLen));
            assert(envWritten.find("stick_pointer = 1\n")!=std::string::npos &&
                   envWritten.find("free_camera = 0\n")!=std::string::npos &&
                   envWritten.find("camera_speed = 5\n")!=std::string::npos);
        }
        unsetenv("PETARI_RSTICK"); unsetenv("GXV_FREE_CAM_SPEED"); sRightStickEnv=-2; sCameraSpeedEnv=-2;
        setenv("GXV_FREE_CAM", "2", 1); sRightStickEnv=-2; assert(port_right_stick_mode()==0 && port_free_camera_enabled()==1);
        setenv("GXV_FREE_CAM", "0", 1); sRightStickEnv=-2; assert(port_right_stick_mode()==1 && port_free_camera_enabled()==0);
        unsetenv("GXV_FREE_CAM"); sRightStickEnv=-2;
        port_tap_jump_set(0); port_invert_camera_set(2); port_star_bit_reach_set(3); port_camera_speed_set(9); port_right_stick_mode_set(0);
        const int len2=ZoomFileText(text,sizeof text);
        contents.assign(text,static_cast<size_t>(len2)); reset();
        assert(port_zoom_tenths()==18 && port_rear_aim_enabled()==1 && port_stick_pointer_enabled()==0 && port_free_camera_enabled()==1 &&
               port_camera_speed()==9 && port_star_bit_reach()==3 && port_tap_jump_enabled()==0 && port_invert_camera_enabled()==2);
        // a file of 2.x/3.0 (no free_camera/camera_speed keys) is read with the new 3.0.1 defaults (Free Camera ON, speed 5 on handheld Vita)
        contents="zoom = 1.3\nrear_aim = 1\nstick_pointer = 0\nstar_bit_reach = 2\n"; reset();
        assert(port_zoom_tenths()==13 && port_rear_aim_enabled()==1 && port_free_camera_enabled()==1 && port_camera_speed()==5 && port_tap_jump_enabled()==1 && port_invert_camera_enabled()==0);
        // CARBON PASS 2 #7: PS TV (externalPad=1) fresh install AND 3.0 upgrade (stick_pointer=0 without free_camera) stay on Pointer (2),
        // while an explicit free_camera = 1 / 0 in 3.0.1 is respected.
        externalPad = 1;
        fileExists = false; reset();
        assert(port_right_stick_mode()==2 && port_stick_pointer_enabled()==1 && port_free_camera_enabled()==0);
        fileExists = true; contents="zoom = 1.5\nstick_pointer = 0\n"; reset();
        assert(port_right_stick_mode()==2 && port_stick_pointer_enabled()==1 && port_free_camera_enabled()==0);
        contents="zoom = 1.5\nstick_pointer = 0\nfree_camera = 1\n"; reset();
        assert(port_right_stick_mode()==0 && port_free_camera_enabled()==1);
        contents="zoom = 1.5\nstick_pointer = 0\nfree_camera = 0\n"; reset();
        assert(port_right_stick_mode()==1 && port_free_camera_enabled()==0 && port_stick_pointer_enabled()==0);
        externalPad = 0;
    }
    std::puts("PASS: production zoom reads all 11 values, file/env precedence, cache, clamps and menu steps; memory-only I/O");
}
