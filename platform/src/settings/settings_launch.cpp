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
// Performance (60 FPS) mode adds what only costs GPU time: no sizes above native, no multisampling, no anisotropic filtering, and aims at 60.
const char* WantMax(const Config& c) {return c.aboveNative&&!c.performance60?nullptr:"2";}
const char* WantMsaa(const Config& c) {return c.antiAliasing==AntiAliasing::Off||c.performance60?"0":nullptr;}
const char* WantAniso(const Config& c) {return c.performance60?"0":nullptr;}
const char* WantTarget(const Config& c) {return c.performance60?"60":nullptr;}
const char* WantSharpen(const Config& c) {return c.sharpness==Sharpness::Off?"off":c.sharpness==Sharpness::Strong?"0.8":nullptr;} // 0.8: the largest amount "auto" ever uses
const char* WantHd(const Config& c) {return c.hdTextures?nullptr:"0";}
const char* WantFxStep(const Config& c) {return c.prioritizeMario&&c.effects==EffectsQuality::Adaptive?nullptr:"0";}
const char* WantCharaAuto(const Config& c) {return c.prioritizeMario?nullptr:"0";}
const char* WantDof(const Config& c) {return c.depthOfField?nullptr:"0";}
const char* WantCore(const Config& c) {return c.coreHelp?nullptr:"0";}
// Tilt (Star Ball, ray surfing): the percentage as text ("100", the game's own, sets nothing) and the left stick as the source.
const char* WantTiltGain(const Config& c) {
    static constexpr const char* text[]={"50","75",nullptr,"125","150","175","200"};
    return tilt::ValidPercent(c.tiltPercent)?text[(c.tiltPercent-tilt::kMinPercent)/tilt::kStepPercent]:nullptr;
}
const char* WantTiltStick(const Config& c) {return c.tiltStick?"1":nullptr;}
constexpr Choice kChoices[]={
    {"GXV_AUTOQ_MAX","8",nullptr,WantMax},{"GXV_AUTOQ_MSAA","1",nullptr,WantMsaa},{"GXV_SHARPEN","auto",nullptr,WantSharpen},{"GXV_HDTEX","1",nullptr,WantHd},
    {"GXV_AUTOQ_FXSTEP","2","2",WantFxStep},{"GXV_CHARAA_AUTO","2","2",WantCharaAuto},{"GXV_DOF","1",nullptr,WantDof},{"GXV_CORE3_IO","1","1",WantCore},
    {"GXV_AUTOQ_TARGET","30","30",WantTarget},{"GXV_AUTOQ_ANISO","1","1",WantAniso},
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
    if(!std::strcmp(key,"GXV_AUTOQ_TARGET"))return r=="60"?"Dynamic quality aims for 60":r=="30"?"Dynamic quality holds 30":"Dynamic quality target "+r;
    if(!std::strcmp(key,"GXV_AUTOQ_ANISO"))return r=="0"?"Anisotropic filtering off":"Anisotropic filtering "+r;
    if(!std::strcmp(key,"GXV_TILT_GAIN"))return "Tilt sensitivity "+r+"%";
    if(!std::strcmp(key,"GXV_TILT_STICK"))return r=="0"?"Tilt from the motion sensor":"Tilt from the left stick";
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
    if(!std::getenv("GXV_FPS_LIMIT")&&config.fpsLimit==30)setenv("GXV_FPS_LIMIT","30",0);
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
