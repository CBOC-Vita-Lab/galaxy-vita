#include "port/settings_launch.h"
#include "port/port.h"
#include "port/reduced_motion.h"
#include "port/tilt_gain.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace port::settings {
namespace {
bool Space(char c) {return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\v'||c=='\f';}
// The Display page's picture and speed choices. `want` is the value the choice needs (null: the game's own default, nothing to set);
// `neutral` is what the switch reads as when it is left alone, so an env.txt value equal to it agrees with that choice. `startup`
// is a value the game's start-up code (vita/vgx-game/entry.cpp) sets itself before Settings runs: seeing exactly that value means
// "not set by env.txt", so the menu's choice may replace it; any other value is env.txt's and wins, as an override.
struct Choice {const char* key; const char* neutral; const char* startup; const char* (*want)(const Config&);};
// Rung 2 is the game's own 640x456 picture (vgx_autoq.h kToday): the ladder's highest step when Sharper when possible is off.
// Picture mode (player_settings.h Config): Quality is the default fpsLimit 0 without performance60; it runs under the fixed 30 cap.
bool Quality(const Config& c) {return c.fpsLimit==0&&!c.performance60;}
// Performance mode: sizes above the game's own up to the screen's 960x544 (rung 6) when there is clear room at 60, never the
// multisampled rungs (their depth traffic is what a 16.7 ms picture cannot afford); the 60 target; the present's edge smoothing as
// the controller's first step under GPU load (GXV_AUTOQ_EDGESTEP). Quality too: there (GXV_AUTOQ_HOLD30) a GPU miss takes the edge
// smoothing off at whatever size it happens, 960x544 included, before the size gives way (~3.5 ms of GPU at 960x544, gpu960 r82).
// Locked 30 keeps it on always.
const char* WantMax(const Config& c) {return !c.aboveNative?"2":c.performance60?"6":nullptr;}
const char* WantMsaa(const Config& c) {return c.antiAliasing==AntiAliasing::Off||c.performance60?"0":nullptr;}
const char* WantTarget(const Config& c) {return c.performance60?"60":nullptr;}
const char* WantEdgeStep(const Config& c) {return Quality(c)||c.performance60?"1":nullptr;}
// Quality mode under the fixed 30 cap (vgx_backend_gl.cpp, the GXV_FPS_LIMIT=30 floor and start): it starts at the screen's own 960x544
// (rung 6) and may give way down to the game's standard size (rung 2) to hold 30; Locked 30 keeps the backend's own 960x544 floor and
// its 2x multisampled start (rungs 6 and 7).
const char* WantCapFloor(const Config& c) {return Quality(c)?"0":nullptr;}  // (r81: down to 608x346 when the GPU is the limit; 30 first)
const char* WantCapStart(const Config& c) {return Quality(c)?"6":nullptr;}
const char* WantSharpen(const Config& c) {return c.sharpness==Sharpness::Off?"off":c.sharpness==Sharpness::Strong?"0.8":nullptr;} // 0.8: the largest amount "auto" ever uses
const char* WantHd(const Config& c) {return c.hdTextures?nullptr:"0";}
const char* WantFxStep(const Config& c) {return c.prioritizeMario&&c.effects==EffectsQuality::Adaptive?nullptr:"0";}
const char* WantCharaAuto(const Config& c) {return c.prioritizeMario?nullptr:"0";}
const char* WantDof(const Config& c) {return c.depthOfField?nullptr:"0";}
const char* WantCore(const Config& c) {return c.coreHelp?nullptr:"0";}
const char* WantGcCore(const Config& c) {return c.coreHelp?nullptr:"2";}      // Off: vitaGL's collector back on core 2 (1.2)
const char* WantAudioCore(const Config& c) {return c.coreHelp?nullptr:"0";}   // Off: the audio mixer on cores 1-2 (1.2)
const char* WantGxm(const Config& c) {return c.gxmDirect?nullptr:"0";}
const char* WantOverlap(const Config& c) {return !c.logicOverlap?"1":c.overlapMax?"4":nullptr;} // Off: 1.2's wait at the start of update(); Max: 3
const char* WantLazy(const Config& c) {return c.lazyMaterials?nullptr:"0";}
const char* WantAnimLod(const Config& c) {return c.farAnimation==FarAnimation::Full?"0":nullptr;}
const char* WantAnimDist(const Config& c) {return c.farAnimation==FarAnimation::Aggressive?"1000":nullptr;}  // (Reduced: 3000, perf.cpp)
const char* WantSlip(const Config& c) {return c.fullSpeed?"0":nullptr;}
const char* WantSkipRun(const Config& c) {return c.fullSpeed?"5":nullptr;}
// Tilt (Star Ball, ray surfing): the percentage as text ("100", the game's own, sets nothing) and the left stick as the source.
const char* WantTiltGain(const Config& c) {
    static constexpr const char* text[tilt::kLevels]={"50","63","75","88",nullptr,"120","140","160","180","200"};   // the percentages of port/tilt_gain.h; level 5 is the game's own
    return tilt::ValidLevel(c.tiltLevel)?text[c.tiltLevel-1]:nullptr;
}
const char* WantTiltStick(const Config& c) {return c.tiltStick?"1":nullptr;}
constexpr Choice kChoices[]={
    {"GXV_AUTOQ_MAX","8",nullptr,WantMax},{"GXV_AUTOQ_MSAA","1",nullptr,WantMsaa},{"GXV_SHARPEN","auto",nullptr,WantSharpen},{"GXV_HDTEX","1",nullptr,WantHd},
    {"GXV_AUTOQ_FXSTEP","3","3",WantFxStep},{"GXV_CHARAA_AUTO","1","1",WantCharaAuto},{"GXV_DOF","1",nullptr,WantDof},{"GXV_CORE3_IO","1","1",WantCore},
    {"GXV_CORE3","auto",nullptr,WantCore},   // Core plan Off: nothing on the fourth core and no probe (port/core_layout.h)
    {"GXV_GC_CORE","1",nullptr,WantGcCore},{"GXV_AUDIO_THREAD_CORE","1","1",WantAudioCore},{"GXV_GXM_DIRECT","1","1",WantGxm},
    {"GXV_DRAWDONE_DEFER","2","2",WantOverlap},{"GXV_DL_LAZY","2",nullptr,WantLazy},{"GXV_ANIM_LOD","1",nullptr,WantAnimLod},{"GXV_ANIM_LOD_DIST","3000",nullptr,WantAnimDist},
    {"GXV_SLIP","1","1",WantSlip},{"PETARI_FRAMESKIP","3",nullptr,WantSkipRun},
    {"GXV_AUTOQ_TARGET","30","30",WantTarget},{"GXV_AUTOQ_EDGESTEP","0",nullptr,WantEdgeStep},
    {"GXV_AUTOQ_LOCKED30_FLOOR","6",nullptr,WantCapFloor},{"GXV_AUTOQ_LOCKED30_START","7",nullptr,WantCapStart},
    {"GXV_TILT_GAIN","100",nullptr,WantTiltGain},{"GXV_TILT_STICK","0",nullptr,WantTiltStick}};
bool SameValue(const char* a,const char* b) {
    if(std::strcmp(a,b)==0)return true;
    char* ea=nullptr;char* eb=nullptr;const long x=std::strtol(a,&ea,0),y=std::strtol(b,&eb,0);
    return ea!=a&&eb!=b&&!*ea&&!*eb&&x==y;
}
std::string ChoiceLabel(const char* key,const char* raw) {
    const std::string r=raw;
    if(!std::strcmp(key,"GXV_AUTOQ_MAX"))return "Highest picture size step "+r;
    if(!std::strcmp(key,"GXV_AUTOQ_MSAA"))return r=="0"?"No multisampling":"Multisampling allowed";
    if(!std::strcmp(key,"GXV_SHARPEN"))return "Sharpening "+r;
    if(!std::strcmp(key,"GXV_HDTEX"))return r=="0"?"HD textures off":"HD textures "+r;
    if(!std::strcmp(key,"GXV_AUTOQ_FXSTEP"))return "Load steps "+r;
    if(!std::strcmp(key,"GXV_CHARAA_AUTO"))return r=="0"?"Mario edge smoothing off":"Mario edge smoothing "+r;
    if(!std::strcmp(key,"GXV_DOF"))return r=="0"?"Depth of field off":"Depth of field "+r;
    if(!std::strcmp(key,"GXV_TILT_GAIN"))return "Tilt sensitivity "+r+"%";
    if(!std::strcmp(key,"GXV_TILT_STICK"))return r=="0"?"Tilt from the motion sensor":"Tilt from the left stick";
    if(!std::strcmp(key,"GXV_GC_CORE"))return "Collector on core "+r;
    if(!std::strcmp(key,"GXV_CORE3"))return r=="0"?"Fourth core off":r=="1"?"Fourth core: every part, when available":"Fourth core "+r;
    if(!std::strcmp(key,"GXV_AUDIO_THREAD_CORE"))return r=="0"?"Audio mixer on cores 1-2":"Audio mixer core plan "+r;
    if(!std::strcmp(key,"GXV_GXM_DIRECT"))return r=="0"?"Standard renderer":"Direct renderer "+r;
    if(!std::strcmp(key,"GXV_DRAWDONE_DEFER"))return "Draw-done wait mode "+r;
    if(!std::strcmp(key,"GXV_DL_LAZY"))return r=="0"?"Materials every turn":"Material updates mode "+r;
    if(!std::strcmp(key,"GXV_ANIM_LOD"))return r=="0"?"Full far animation":"Far animation "+r;
    if(!std::strcmp(key,"GXV_ANIM_LOD_DIST"))return "Far animation from "+r+" units";
    if(!std::strcmp(key,"GXV_SLIP"))return r=="0"?"Game speed: full (no slowdown)":"Game speed: slowdown allowed";
    if(!std::strcmp(key,"PETARI_FRAMESKIP"))return "Up to "+r+" pictures skipped in a row";
    if(!std::strcmp(key,"GXV_AUTOQ_TARGET"))return r=="60"?"Dynamic quality aims for 60":r=="30"?"Dynamic quality holds 30":"Dynamic quality target "+r;
    if(!std::strcmp(key,"GXV_AUTOQ_EDGESTEP"))return r=="0"?"Edge smoothing kept under load":"Edge smoothing given up first under load";
    if(!std::strcmp(key,"GXV_AUTOQ_LOCKED30_FLOOR"))return "Smallest picture size step at 30 FPS "+r;
    if(!std::strcmp(key,"GXV_AUTOQ_LOCKED30_START"))return "First picture size step at 30 FPS "+r;
    return r=="0"?"System core help off":"System core help "+r;
}
bool Tokens(const char* p,size_t length,std::vector<std::string>& out) {
    std::string token;char quote=0;bool started=false;
    for(size_t i=0;i<length;++i) {
        char ch=p[i];
        if(quote) {if(ch==quote)quote=0;else token+=ch;}
        else if(ch=='\''||ch=='"') {quote=ch;started=true;}
        else if(Space(ch)) {if(started){out.push_back(token);token.clear();started=false;}}
        else {token+=ch;started=true;}
    }
    if(quote)return false;
    if(started)out.push_back(token);
    return true;
}
}
LaunchRequest ParseLaunch(int argc,const char* const* argv,const char* parameter,size_t bytes) {
    bool settings=false;
    for(int i=0;argv&&i<argc;++i) if(argv[i]) {
        if(std::strcmp(argv[i],"-play")==0)return LaunchRequest::Play;
        if(std::strcmp(argv[i],"-settings")==0)settings=true;
    }
    if(parameter&&bytes) {
        if(bytes>1024)return LaunchRequest::Malformed;
        const auto* end=static_cast<const char*>(std::memchr(parameter,0,bytes));
        if(!end)return LaunchRequest::Malformed;
        size_t length=static_cast<size_t>(end-parameter);
        if(length>=5&&std::memcmp(parameter,"psla:",5)==0) {parameter+=5;length-=5;}
        std::vector<std::string> tokens;if(!Tokens(parameter,length,tokens))return LaunchRequest::Malformed;
        for(const auto& t:tokens)if(t=="-play")return LaunchRequest::Play;
        for(const auto& t:tokens)if(t=="-settings")settings=true;
    } else if(parameter||bytes)return LaunchRequest::Malformed;
    return settings?LaunchRequest::Settings:LaunchRequest::Play;
}
LanguageCatalog ReadLanguages() {
    LanguageCatalog catalog;
    const int count=port_language_count();
    for(int i=0;i<count&&i<32;++i) {
        const char* name=port_language_name(i);
        if(name&&name[0]&&std::find(catalog.names.begin(),catalog.names.end(),name)==catalog.names.end())catalog.names.emplace_back(name);
    }
    return catalog;
}
std::vector<Override> ApplyStartup(const Config& config,bool enabled) {
    std::vector<Override> overrides;if(!enabled)return overrides;
    for(const auto* key:{"GXV_FPS_LIMIT","GXV_PACE","GXV_FXHALF","GXV_AUTOQ","GXV_AUTOQ_LEVEL","GXV_AUTOQ_CYCLE","PETARI_LANGUAGE","GXV_REMAP","GXV_MOUSE_AIM","GXV_REDUCED_MOTION"}) {
        if(const char* raw=std::getenv(key)) {
            // Remapping is on by default (the start-up default sets GXV_REMAP=1); only turning it off is an override.
            if(std::strcmp(key,"GXV_REMAP")==0&&std::strcmp(raw,"1")==0)continue;
            // (GXV_AUTOQ_TARGET is a Picture mode choice in the table below: the start-up 30 is Quality's and Locked 30's, 60 Performance's)
            // Reduced motion is an override only when env.txt disagrees with the Settings choice (the game reads it as on or off).
            if(std::strcmp(key,"GXV_REDUCED_MOTION")==0&&motion::ParseSwitch(raw)==config.reducedMotion)continue;
            std::string effective=raw;
            if(std::strcmp(key,"GXV_FPS_LIMIT")==0) effective=std::strcmp(raw,"30")==0?"30 FPS requested (driver capability required)":"Existing picture-rate path";
            if(std::strcmp(key,"GXV_REDUCED_MOTION")==0) effective=motion::ParseSwitch(raw)?"Reduced motion on":"Reduced motion off";
            if(std::strcmp(key,"GXV_REMAP")==0) effective=std::strcmp(raw,"0")==0?"Remapping disabled":"Remapping off (unknown value)";
            overrides.push_back({key,raw,effective});
        }
    }
    // Picture and speed choices: one start-up pass over the table. Env values the menu agrees with are not overrides.
    for(const auto& choice:kChoices) {
        const char* raw=std::getenv(choice.key);const char* want=choice.want(config);
        const bool fromStartup=raw&&choice.startup&&std::strcmp(raw,choice.startup)==0;
        if(raw&&!fromStartup) {
            if(!SameValue(raw,want?want:choice.neutral))overrides.push_back({choice.key,raw,ChoiceLabel(choice.key,raw)});
            continue;
        }
        if(want)setenv(choice.key,want,1);
    }
    // Locked 30 and Quality both run under the fixed 30 cap (when the driver has it; otherwise the game logs that it keeps its old pacing).
    if(!std::getenv("GXV_FPS_LIMIT")&&(config.fpsLimit==30||Quality(config)))setenv("GXV_FPS_LIMIT","30",0);
    // (Locked 30 is the quality mode: it starts at the screen's own 960x544 and keeps its size across fades and loads; see
    // vgx_backend_gl.cpp, GXV_AUTOQ_START and GXV_AUTOQ_WIPE_LOCKED30. An overload still steps it down.)
    // Steady 30, the aggressive opt-in mode: the fixed 30 FPS ceiling (never the 30/20 adaptive cadence),
    // dynamic quality aimed at 30 that lowers resolution only as a last resort; effects stay full size. 60 Hz game
    // turns are unchanged. env.txt values win over each part.
    if(config.fpsLimit==kSteady30&&!std::getenv("GXV_FPS_LIMIT")) {setenv("GXV_FPS_LIMIT","30",0);setenv("GXV_AUTOQ_TARGET","30",0);} // full-size effects: 30 is held by speed, not by smaller effects (GXV_FXHALF stays a test switch)
    if(!std::getenv("GXV_AUTOQ")&&!std::getenv("GXV_AUTOQ_LEVEL")&&!std::getenv("GXV_AUTOQ_CYCLE")&&config.autoQuality!=AutoQuality::Default)
        setenv("GXV_AUTOQ",config.autoQuality==AutoQuality::Enabled?"1":"0",0);
    // Mouse-style Star Bit pointer aiming (vita/runtime/input.cpp reads GXV_MOUSE_AIM once); an env.txt value wins.
    if(config.mouseAim)setenv("GXV_MOUSE_AIM","1",0);
    // Reduced motion in play (port/reduced_motion.h reads GXV_REDUCED_MOTION once: less camera shake, softer short flashes); an env.txt value wins.
    if(config.reducedMotion)setenv("GXV_REDUCED_MOTION","1",0);
    if(!std::getenv("PETARI_LANGUAGE")&&!config.language.empty()) {
        const auto catalog=ReadLanguages();
        // An ordinary launch may not have discovered the disc yet; the existing
        // discovery code validates the wanted canonical name against its catalog.
        if(catalog.names.empty()||std::find(catalog.names.begin(),catalog.names.end(),config.language)!=catalog.names.end())port_language_set(config.language.c_str());
    }
    return overrides;
}
}
