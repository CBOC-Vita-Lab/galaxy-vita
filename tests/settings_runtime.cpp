// Actual native adapter; only SDK/device and the independently-tested menu are substituted.
#include <cassert>
#include <chrono>
#include <cstring>
#include <map>
#include <thread>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include "player_settings_font_fixture.h"
#include "settings_font_allocation_fixture.h"
#include "../vita/vgx-game/settings_runtime.cpp"

namespace fixture {
std::string parameter;
unsigned parameterReads=0, fileReads=0, discProbes=0, opens=0, frees=0, menus=0, restarts=0;
unsigned guestRearms=0, hostRearms=0, restoreAttempts=0, resumed=0;
unsigned fontOpens=0, fontCloses=0, dirReads=0;
bool fontExpected=false, shortReads=false, prematureEof=false, failRead=false, failClose=false;
bool playStation=false;
bool exerciseFontFailures=false;
bool failDirectoryRead=false, failDirectoryClose=false;
std::map<std::string,std::vector<uint8_t>> files;
std::map<std::string,std::vector<std::string>> directories;
std::map<int,std::pair<std::string,size_t>> fileHandles, directoryHandles;
bool title=false, renderer=false, paused=false, audio=false;
bool failAllocate=false, failPresent=false, failRenderer=false;
bool failScanout=false, restoreSubmitted=false, gameplayHeld=true;
unsigned failRestoreSet=0, failRestoreWait=0;
void* original=nullptr;
int clock=0;
int interrupts=1;
std::map<int,void*> blocks;
SceDisplayFrameBuf current{};
port::settings::ExitIntent nextIntent=port::settings::ExitIntent::Exit;
}
extern "C" int sceAppMgrGetAppParam(char* data) {
    ++fixture::parameterReads;
    std::strncpy(data,fixture::parameter.c_str(),1024);fixture::parameter.clear();return 0;
}
extern "C" int sceAppMgrLoadExec(const char* path,char* const argv[],const SceAppMgrExecOptParam*) {
    assert(std::strcmp(path,"app0:eboot.bin")==0 && std::strcmp(argv[0],"-play")==0 && !argv[1]);
    assert(!fixture::renderer && !fixture::audio && fixture::interrupts==1);
    ++fixture::restarts;fixture::nextIntent=port::settings::ExitIntent::Exit;return -1;
}
extern "C" SceUID sceKernelAllocMemBlock(const char*,SceKernelMemBlockType,SceSize bytes,SceKernelAllocMemBlockOpt*) {
    if(fixture::failAllocate)return -1;
    const int id=++fixture::opens;fixture::blocks[id]=std::calloc(1,bytes);return id;
}
extern "C" int sceKernelGetMemBlockBase(SceUID id,void** out) {*out=fixture::blocks[id];return 0;}
extern "C" int sceKernelFreeMemBlock(SceUID id) {
    assert(fixture::current.base!=fixture::blocks[id]);std::free(fixture::blocks[id]);fixture::blocks.erase(id);++fixture::frees;return 0;
}
extern "C" int sceDisplayGetFrameBuf(SceDisplayFrameBuf* out,SceDisplaySetBufSync) {if(fixture::failScanout)return -1;*out=fixture::current;return 0;}
void AssertRestoreHeld() {
    assert(fixture::renderer && fixture::audio && fixture::paused && fixture::clock && fixture::interrupts==0);
    assert(sInputHold.Requested() && port_settings_active());
    assert(!fixture::guestRearms && !fixture::hostRearms && !fixture::resumed && fixture::blocks.size()==1);
}
extern "C" int sceDisplaySetFrameBuf(const SceDisplayFrameBuf* in,SceDisplaySetBufSync) {
    if(fixture::failPresent)return -1;
    fixture::restoreSubmitted=in && in->base==fixture::original;
    if(fixture::restoreSubmitted) {
        ++fixture::restoreAttempts;
        if(fixture::failRestoreSet){AssertRestoreHeld();--fixture::failRestoreSet;return -1;}
    }
    fixture::current=in?*in:SceDisplayFrameBuf{};return 0;
}
extern "C" int sceDisplayWaitVblankStart() {
    if(fixture::restoreSubmitted && fixture::failRestoreWait){AssertRestoreHeld();--fixture::failRestoreWait;return -1;}
    return 0;
}
extern "C" SceUInt64 sceKernelGetProcessTimeWide() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" int sceKernelDelayThread(SceUInt us) {std::this_thread::sleep_for(std::chrono::microseconds(us));return 0;}
extern "C" int sceIoGetstat(const char* path,SceIoStat* info) {
    ++fixture::fileReads;*info={};
    if(fixture::files.count(path)){info->st_mode=SCE_S_IFREG;info->st_size=fixture::files[path].size();return 0;}
    if(fixture::directories.count(path)){info->st_mode=SCE_S_IFDIR;return 0;}
    return static_cast<int>(0x80010002u);
}
extern "C" SceUID sceIoOpen(const char* path,int flags,SceMode) {
    assert(flags==SCE_O_RDONLY);
    if(!fixture::files.count(path))return static_cast<int>(0x80010002u);
    const int handle=100+fixture::fontOpens++;fixture::fileHandles[handle]={path,0};return handle;
}
extern "C" int sceIoRead(SceUID handle,void* out,SceSize size) {
    auto& [path,offset]=fixture::fileHandles.at(handle);const auto& data=fixture::files.at(path);
    if(fixture::failRead)return -1;
    const size_t length=fixture::prematureEof?data.size()/2:data.size();
    const size_t count=std::min<size_t>(std::min<size_t>(size,length-offset),fixture::shortReads?17:size);
    std::memcpy(out,data.data()+offset,count);offset+=count;return count;
}
extern "C" int sceIoWrite(SceUID,const void*,SceSize) {assert(false);return -1;}
extern "C" int sceIoSyncByFd(SceUID,int) {assert(false);return -1;}
extern "C" int sceIoClose(SceUID handle) {assert(fixture::fileHandles.erase(handle)==1);++fixture::fontCloses;return fixture::failClose?-1:0;}
extern "C" SceUID sceIoDopen(const char* path) {
    if(!fixture::directories.count(path))return -1;
    const int handle=200+fixture::directoryHandles.size();fixture::directoryHandles[handle]={path,0};return handle;
}
extern "C" int sceIoDread(SceUID handle,SceIoDirent* out) {
    if(fixture::failDirectoryRead)return -1;
    ++fixture::dirReads;auto& [path,index]=fixture::directoryHandles.at(handle);const auto& entries=fixture::directories.at(path);
    if(index==entries.size())return 0;
    *out={};const auto& name=entries[index++];assert(name.size()<sizeof(out->d_name));std::strcpy(out->d_name,name.c_str());
    out->d_stat.st_mode=fixture::directories.count(path+"/"+name)?SCE_S_IFDIR:SCE_S_IFREG;return 1;
}
extern "C" int sceIoDclose(SceUID handle) {assert(fixture::directoryHandles.erase(handle)==1);return fixture::failDirectoryClose?-1:0;}
extern "C" int sceIoRename(const char*,const char*) {assert(false);return -1;}
extern "C" int sceIoRemove(const char*) {assert(false);return -1;}
extern "C" int port_vita_input_raw_init() {return 1;}
bool port_vita_input_raw_poll(PortVitaRawInput& out) {
    out={};out.source=fixture::playStation?1:0;out.controllerType=fixture::playStation?SCE_CTRL_TYPE_DS4:SCE_CTRL_TYPE_PHY;
    out.caps=port::controls::NominalCaps(port::controls::ProfileId::Handheld);out.caps.connected=true;return true;
}
bool port_vita_input_mapping_available() {return true;}
extern "C" void port_vita_input_settings_rearm() { assert(fixture::interrupts==0 && sInputHold.Requested());++fixture::hostRearms;fixture::gameplayHeld=false; }
extern "C" void port_input_settings_rearm() { assert(fixture::interrupts==0 && sInputHold.Requested());++fixture::guestRearms;fixture::gameplayHeld=false; }
extern "C" int port_is_paused() {return fixture::paused;}
extern "C" void port_set_paused(int value) {fixture::paused=value!=0;}
extern "C" int port_settings_time_pause(int value) {const int old=fixture::clock;fixture::clock=value;return old;}
extern "C" int port_vita_audio_is_held() {return fixture::audio;}
extern "C" int port_vita_audio_hold(int value,unsigned) {fixture::audio=value!=0;return 1;}
extern "C" int port_vgx_settings_hold(unsigned,unsigned) {if(fixture::failRenderer)return 0;fixture::renderer=true;return 1;}
extern "C" void port_vgx_settings_release(unsigned) {fixture::renderer=false;}
extern "C" int port_vgx_vita_fixed_cap_supported() {return 1;}
extern "C" int port_boot_title_is_visible() {return fixture::title;}
extern "C" int port_vgx_vita_splash_active() {return !fixture::title;}
extern "C" int OSDisableInterrupts() {const int old=fixture::interrupts;fixture::interrupts=0;return old;}
extern "C" int OSRestoreInterrupts(int state) {const int old=fixture::interrupts;fixture::interrupts=state;return old;}
extern "C" void port_dvd_set_root(const char*) {++fixture::discProbes;}
extern "C" const char* port_dvd_root() {return "ux0:data/smg/game";}
extern "C" int port_language_count() {return 1;}
extern "C" const char* port_language_name(int) {return "english";}
extern "C" void port_language_set(const char*) {}
extern "C" void port_host_alloc_scope_enter() {}
extern "C" void port_host_alloc_scope_leave() {}
extern "C" void port_log_summary(const char* message,...) {if(std::strstr(message,"gameplay resumed"))++fixture::resumed;}
namespace port::settings {
RunResult Run(SettingsSession& session) {
    ++fixture::menus;
    assert(session.surface.width==960 && session.surface.height==544 && session.surface.pixels);
    assert(session.active.fpsLimit==0);
    assert(session.detectedProfile==(fixture::playStation?port::controls::ProfileId::PlayStation:port::controls::ProfileId::Handheld));
    assert(bool(session.fonts.menu)==fixture::fontExpected && bool(session.fonts.message)==fixture::fontExpected);
    if(session.mode==OpenMode::Running)assert(fixture::renderer && fixture::audio && fixture::paused && fixture::interrupts==0);
    if(fixture::exerciseFontFailures) {
        for(unsigned failure=1;failure<=7;++failure) {
            FontAllocationProbe probe;probe.failAt=failure;
            const unsigned opened=fixture::fontOpens,closed=fixture::fontCloses;
            {
                const auto rejected=LoadMenuFonts(probe.allocator());
                assert(!rejected.menu.valid()&&!rejected.message.valid());
            }
            assert(!probe.live && probe.calls==failure);
            assert(fixture::fontOpens-opened==(failure==1?0u:1u) && fixture::fontCloses-closed==(failure==1?0u:1u));
            assert(fixture::renderer && fixture::audio && fixture::paused && fixture::clock && fixture::interrupts==0);
            assert(sInputHold.Requested() && port_settings_active() && !fixture::guestRearms && !fixture::hostRearms);
        }
    }
    if(fixture::menus>1 && fixture::original)assert(session.statusMessage.find("Display")!=std::string::npos);
    if(!session.host.present(session.host.context,session.surface))return {session.mode==OpenMode::Running?ExitIntent::Return:ExitIntent::Exit,false};
    return {fixture::nextIntent,fixture::nextIntent==ExitIntent::Play || fixture::nextIntent==ExitIntent::Restart};
}
}
int main(int argc,char** argv) {
    assert(argc==2);
    const std::string mode=argv[1];
    setenv("GXV_SETTINGS",mode=="off"?"0":"1",1);
    fixture::parameter="psla:-settings";
    if(mode=="running-font-oom") {
        fixture::directories["ux0:data/smg/game/files"]={"LayoutData"};
        fixture::directories["ux0:data/smg/game/files/LayoutData"]={"Font.arc"};
        fixture::files["ux0:data/smg/game/files/LayoutData/Font.arc"]=settings_fixture::Archive();
        fixture::fontExpected=fixture::exerciseFontFailures=true;
    }
    if(mode.rfind("font-",0)==0) {
        const std::string root="ux0:data/smg/game", files=root+"/files", layout=files+"/LayoutData";
        fixture::directories[root]={"files"};fixture::directories[files]={"LayoutData"};fixture::directories[layout]={};
        std::string path=layout+"/Font.arc";
        if(mode=="font-bucket" || mode=="font-ambiguous" || mode=="font-directory-read-error" || mode=="font-directory-close-error") {
            fixture::directories[layout]={"_bk02"};fixture::directories[layout+"/_bk02"]={"Font.arc"};path=layout+"/_bk02/Font.arc";
            if(mode=="font-ambiguous"){fixture::directories[layout].push_back("_bk03");fixture::directories[layout+"/_bk03"]={"Font.arc"};fixture::files[layout+"/_bk03/Font.arc"]=settings_fixture::Archive();}
        } else if(mode=="font-cased") {
            fixture::directories.erase(files);fixture::directories.erase(layout);
            fixture::directories[root]={"FILES"};fixture::directories[root+"/FILES"]={"layoutdata"};
            fixture::directories[root+"/FILES/layoutdata"]={"FONT.ARC"};path=root+"/FILES/layoutdata/FONT.ARC";
        }
        if(mode!="font-missing")fixture::files[path]=settings_fixture::Archive();
        if(mode=="font-corrupt")fixture::files[path][0]=0;
        if(mode=="font-oversized")fixture::files[path].resize(8*1024*1024+1);
        if(mode=="font-bounded") {
            fixture::files.erase(path);
            for(unsigned i=0;i<4097;++i)fixture::directories[layout].push_back("unrelated"+std::to_string(i));
            fixture::directories[layout].push_back("_bk02");fixture::directories[layout+"/_bk02"]={"Font.arc"};
            fixture::files[layout+"/_bk02/Font.arc"]=settings_fixture::Archive();
        }
        if(mode=="font-bucket-limit") {
            fixture::files.erase(path);
            for(unsigned i=0;i<129;++i) {
                const std::string name="_bk"+std::to_string(100+i);
                fixture::directories[layout].push_back(name);fixture::directories[layout+"/"+name]={};
            }
        }
        fixture::shortReads=mode=="font-short-read";
        fixture::prematureEof=mode=="font-eof";
        fixture::failRead=mode=="font-read-error";
        fixture::failClose=mode=="font-close-error";
        fixture::failDirectoryRead=mode=="font-directory-read-error";
        fixture::failDirectoryClose=mode=="font-directory-close-error";
        fixture::fontExpected=mode=="font-flat" || mode=="font-bucket" || mode=="font-cased" || mode=="font-short-read";
        assert(port_settings_startup(0,nullptr));assert(fixture::menus==1);
        assert(fixture::fontOpens==fixture::fontCloses && fixture::fileHandles.empty() && fixture::directoryHandles.empty());
        if(mode=="font-missing" || mode=="font-ambiguous" || mode=="font-oversized" || mode=="font-bounded" || mode=="font-bucket-limit" || fixture::failDirectoryRead || fixture::failDirectoryClose)assert(!fixture::fontOpens);
        if(mode=="font-bounded")assert(fixture::dirReads==4096);
    } else if(mode=="off") {
        assert(!port_settings_startup(0,nullptr));assert(!fixture::fileReads && !fixture::menus && !fixture::parameterReads);
    } else if(mode=="allocation-failure") {
        fixture::failAllocate=true;
        assert(port_settings_startup(0,nullptr));assert(fixture::menus==0 && fixture::blocks.empty());
    } else if(mode=="present-failure") {
        fixture::failPresent=true;
        assert(port_settings_startup(0,nullptr));assert(fixture::menus==1 && fixture::frees==1);
    } else if(mode=="cold" || mode=="cold-playstation") {
        fixture::playStation=mode=="cold-playstation";
        assert(port_settings_startup(0,nullptr));assert(fixture::menus==1 && fixture::discProbes==1 && fixture::frees==1);
    } else if(mode=="stale") {
        char play[]="-play";char* args[]={play,nullptr};
        assert(!port_settings_startup(1,args));assert(fixture::parameterReads==1 && !fixture::menus && !fixture::discProbes);
        assert(!fixture::fontOpens && !fixture::dirReads);
    } else if(mode=="restart-failure") {
        fixture::nextIntent=port::settings::ExitIntent::Play;
        assert(port_settings_startup(0,nullptr));assert(fixture::restarts==1 && fixture::menus==2);
    } else {
        fixture::parameter.clear();assert(!port_settings_startup(0,nullptr));
        static uint32_t originalScreen=0;
        fixture::current.base=fixture::original=&originalScreen;
        fixture::nextIntent=port::settings::ExitIntent::Return;
        port_settings_request();port_settings_game_boundary(1);assert(fixture::menus==0);
        fixture::title=true;port_settings_game_boundary(0);assert(fixture::menus==0);
        const bool prior=mode=="running-prior";
        if(prior)fixture::audio=fixture::paused=true;
        if(mode=="running-failure")fixture::failRenderer=true;
        if(mode=="running-scanout-failure")fixture::failScanout=true;
        if(mode=="running-restore-set-failure")fixture::failRestoreSet=4;
        if(mode=="running-restore-wait-failure")fixture::failRestoreWait=4;
        if(mode=="running-restart")fixture::nextIntent=port::settings::ExitIntent::Restart;
        std::atomic<bool> stop{false};
        std::thread input([&] {while(!stop.load()){port_settings_input_service();std::this_thread::sleep_for(std::chrono::milliseconds(1));}});
        port_settings_game_boundary(1);stop=true;input.join();
        const bool acquisitionFailed=fixture::failRenderer || fixture::failScanout;
        const bool restoreFailed=mode=="running-restore-set-failure" || mode=="running-restore-wait-failure";
        assert(fixture::menus==(acquisitionFailed?0u:restoreFailed?2u:1u) && !fixture::renderer && fixture::audio==prior && fixture::paused==prior && fixture::interrupts==1 && !fixture::clock);
        assert(fixture::guestRearms==(acquisitionFailed?0u:1u) && fixture::hostRearms==(acquisitionFailed?0u:1u));
        assert(fixture::gameplayHeld==acquisitionFailed);
        if(restoreFailed)assert(fixture::restoreAttempts>=4 && fixture::frees==1 && fixture::resumed==1);
        if(mode=="running-restart")assert(fixture::restarts==1);
        assert(!port_settings_active());assert(!getenv("GXV_FPS_LIMIT"));
    }
    assert(fixture::blocks.empty());
    assert(fixture::fileHandles.empty() && fixture::directoryHandles.empty() && fixture::fontOpens==fixture::fontCloses);
    std::printf("PASS: actual native settings adapter %s\n",mode.c_str());
}
