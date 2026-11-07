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
    std::puts("PASS: production zoom reads all 11 values, file/env precedence, cache, clamps and menu steps; memory-only I/O");
}
