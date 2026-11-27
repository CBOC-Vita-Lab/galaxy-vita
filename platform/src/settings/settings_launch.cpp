#include "port/settings_launch.h"
#include "port/port.h"
#include "port/quality_set.h"
#include "port/quality30.h"
#include "port/mode_keys.h"
#include "port/perf_defaults.h"   // (pace33 PATCH8)
#include "port/frame_pace.h"
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
// modes26 (12 Nov): two picture modes, Config::Mode30. The hidden rows (Dynamic resolution, Sharper, Prioritize Mario, Anti-aliasing, Effects quality, Preset) are not read here:
// the modes fix what they chose. Performance pins rung 6 (960x544, 1x); Quality starts at rung 7 (960x544, 2x MSAA with GXV_Q30_MSAA=1 from mode_keys.h, stepping down to
// rung 6 1x + edge5 automatically before any detail cut; GXV_Q30_MSAA=0 in env.txt pins rung 6 1x).
//   60 (Performance): GXV_FPS_LIMIT=0 GXV_AUTOQ_TARGET=60 GXV_STEADY30=0 GXV_PERF_BUDGET=1 GXV_EDGE_AA_NATIVE=0 (no edge pass over a 1:1 picture: ~3.5 ms of GPU) and the shed
//        steps as before (GXV_AUTOQ_EDGESTEP=1); the per-stage table's cuts apply (GXV_PICTURE_MODE=60 in the preset code).
//   30 (Quality):     GXV_FPS_LIMIT=30 (the driver's fixed cap: an even 30), target 30, GXV_STEADY30=0 (that machinery is Performance's), GXV_PERF_BUDGET=0, the edge smoothing
//        kept always (GXV_EDGE_AA_NATIVE=1, GXV_AUTOQ_EDGESTEP=0, GXV_PACE23_LOCKED_EDGE=0), no load steps (GXV_AUTOQ_FXSTEP=0), GXV_SMOOTH60=0, and every stage-table
//        key that is a cut is skipped (GXV_PICTURE_MODE=30; exact keys still apply).
bool Is30(const Config& c) {return Mode30(c);}
bool Q30Msaa() {
    if(const char* v=std::getenv("GXV_Q30_MSAA");v&&*v)return std::atoi(v)!=0;
    for(int i=0;i<port::modekeys::kRowCount;++i)
        if(std::strcmp(port::modekeys::kRows[i].key,"GXV_Q30_MSAA")==0)
            return port::modekeys::kRows[i].quality&&std::atoi(port::modekeys::kRows[i].quality)!=0;
    return false;
}
const char* WantMax(const Config& c) {return Is30(c)&&Q30Msaa()?"7":"6";}
const char* WantMin(const Config&) {return "6";}
const char* WantMsaa(const Config& c) {return Is30(c)&&Q30Msaa()?"1":"0";}
const char* WantTarget(const Config& c) {return Is30(c)?"30":"60";}
const char* WantEdgeStep(const Config& c) {return Is30(c)?"0":"1";}
const char* WantLockedEdge(const Config& c) {return Is30(c)?"0":nullptr;}
const char* WantEdgeNative(const Config& c) {return Is30(c)?"1":"0";}
// (rate30, 13 Nov: env.txt's GXV_STEADY30=1 cannot be told from the start-up default "1", so it was replaced by the menu's "0" and
// GXV_RATE_AUTO=1 declared itself ignored. The 60 <-> even-30 controller's own key keeps the steady-30 machinery on in Performance.)
const char* WantSteady(const Config& c) {const char* r=std::getenv("GXV_RATE_AUTO");return !Is30(c)&&r&&std::atoi(r)!=0?"1":"0";}
const char* WantBudget(const Config& c) {return Is30(c)?"0":"1";}
const char* WantSmooth(const Config& c) {return Is30(c)?"0":nullptr;}   // (the GXV_SMOOTH60 ladder stays off by default; in 30 it can never run)
const char* WantMode(const Config& c) {return Is30(c)?"30":"60";}
// The 30 mode's size window: the backend's Locked 30 floor at 960x544 (rung 6), start at rung 7 (2x MSAA) when GXV_Q30_MSAA=1 else rung 6.
const char* WantCapFloor(const Config& c) {return Is30(c)?"6":nullptr;}
const char* WantCapStart(const Config& c) {return Is30(c)?(Q30Msaa()?"7":"6"):nullptr;}
const char* WantSharpen(const Config& c) {return c.sharpness==Sharpness::Off?"off":c.sharpness==Sharpness::Strong?"0.8":nullptr;} // 0.8: the largest amount "auto" ever uses
const char* WantHd(const Config& c) {return c.hdTextures?nullptr:"0";}
const char* WantFxStep(const Config& c) {return Is30(c)?"0":nullptr;}   // (30: the effects/backgrounds step stays off; only a severe overload may halve effects and blur, owner decision 18 Nov: Quality may cut effects to keep a steady 30; 60: the shed ladder as the game starts it)
const char* WantCharaAuto(const Config&) {return nullptr;}
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
const char* WantSafeSaves(const Config& c) {return c.safeSaves?"1":nullptr;}   // SMG_ATOMIC_SAVES (platform/src/nand/nand.cpp): off unless asked
const char* WantTiltStick(const Config& c) {return c.tiltStick?"1":nullptr;}
constexpr Choice kChoices[]={
    {"GXV_AUTOQ_MAX","8",nullptr,WantMax},{"GXV_AUTOQ_MIN","0","0",WantMin},{"GXV_STEADY30","0","1",WantSteady},{"GXV_PERF_BUDGET","0",nullptr,WantBudget},
    {"GXV_EDGE_AA_NATIVE","1",nullptr,WantEdgeNative},{"GXV_PACE23_LOCKED_EDGE","1",nullptr,WantLockedEdge},{"GXV_SMOOTH60","0","0",WantSmooth},{"GXV_PICTURE_MODE","60",nullptr,WantMode},{"GXV_AUTOQ_MSAA","1",nullptr,WantMsaa},{"GXV_SHARPEN","auto",nullptr,WantSharpen},{"GXV_HDTEX","1",nullptr,WantHd},
    {"GXV_AUTOQ_FXSTEP","3","3",WantFxStep},{"GXV_CHARAA_AUTO","1","1",WantCharaAuto},{"GXV_DOF","1",nullptr,WantDof},{"GXV_CORE3_IO","1","1",WantCore},
    {"GXV_CORE3","auto",nullptr,WantCore},   // Core plan Off: nothing on the fourth core and no probe (port/core_layout.h)
    {"GXV_GC_CORE","1",nullptr,WantGcCore},{"GXV_AUDIO_THREAD_CORE","1","1",WantAudioCore},{"GXV_GXM_DIRECT","1","1",WantGxm},
    {"GXV_DRAWDONE_DEFER","2","2",WantOverlap},{"GXV_DL_LAZY","2",nullptr,WantLazy},{"GXV_ANIM_LOD","1",nullptr,WantAnimLod},{"GXV_ANIM_LOD_DIST","3000",nullptr,WantAnimDist},
    {"GXV_SLIP","1","1",WantSlip},{"PETARI_FRAMESKIP","3",nullptr,WantSkipRun},
    {"GXV_AUTOQ_TARGET","30","30",WantTarget},{"GXV_AUTOQ_EDGESTEP","0",nullptr,WantEdgeStep},
    {"GXV_AUTOQ_LOCKED30_FLOOR","5",nullptr,WantCapFloor},{"GXV_AUTOQ_LOCKED30_START","7",nullptr,WantCapStart},
    {"GXV_TILT_GAIN","100",nullptr,WantTiltGain},{"GXV_TILT_STICK","0",nullptr,WantTiltStick},{"SMG_ATOMIC_SAVES","0",nullptr,WantSafeSaves}};
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
    if(!std::strcmp(key,"SMG_ATOMIC_SAVES"))return r=="0"?"Safe saves off":"Safe saves on";
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
    if(!std::strcmp(key,"GXV_AUTOQ_MIN"))return "Lowest picture size step "+r;
    if(!std::strcmp(key,"GXV_STEADY30"))return r=="0"?"Steady 30 fallback off":"Steady 30 fallback on";
    if(!std::strcmp(key,"GXV_PERF_BUDGET"))return r=="0"?"Far-detail budget off":"Far-detail budget on";
    if(!std::strcmp(key,"GXV_EDGE_AA_NATIVE"))return r=="0"?"No edge smoothing at 960x544":"Edge smoothing at 960x544";
    if(!std::strcmp(key,"GXV_PACE23_LOCKED_EDGE"))return r=="0"?"Edge smoothing kept at 30":"Edge smoothing may be given up at 30";
    if(!std::strcmp(key,"GXV_SMOOTH60"))return r=="0"?"Smooth-60 ladder off":"Smooth-60 ladder on";
    if(!std::strcmp(key,"GXV_PICTURE_MODE"))return "Stage table mode "+r;
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
// gfx26 (GXV_QUALITY_SET, port/quality_set.h): the Picture mode's level of the picture-quality umbrella, applied once after the choices above
// (it overwrites their and entry.cpp's defaults for the members env.txt left alone; entry.cpp took the snapshot of env.txt's keys). The census text
// is kept for the basic log (settings_runtime.cpp prints it right after the "settings: picture mode" line).
namespace {
std::string sQualityCensus;
std::string sQuality30Census;   // (q30-26)
std::string sModeKeysCensus;    // (deadfix27)
std::string sPerfDefaultsCensus;   // (pace33 PATCH8)
}
const std::string& QualitySetCensus() {return sQualityCensus;}
const std::string& Quality30Census() {return sQuality30Census;}
const std::string& ModeKeysCensus() {return sModeKeysCensus;}
const std::string& PerfDefaultsCensus() {return sPerfDefaultsCensus;}
namespace {
bool ApplyQualitySetMode(port::qualityset::Mode mode) {
    namespace qs=port::qualityset;
    if(qs::Applied())return false;
    const qs::Parsed p=qs::Resolve(std::getenv("GXV_QUALITY_SET"),mode);
    qs::Applied()=true;
    if(p.level==0&&!p.fromEnv)return false;   // not asked for: no census line, nothing touched
    const char* pv=std::getenv("GXV_QUALITY_SET_PROVEN");
    uint32_t mask=qs::UserMask();
    if(!qs::Snapshotted()) {                  // (no snapshot of env.txt: whatever is in the environment counts as the user's)
        mask=0;
        for(int i=0;i<qs::kMemberCount;++i) {const char* v=std::getenv(qs::kMembers[i].key);if(v&&*v)mask|=1u<<i;}
    }
    const qs::Result r=qs::Apply(p.level,mode,p.bad?"unreadable value, treated as 0":p.fromEnv?"from env.txt":"proposed default",pv&&*pv&&std::strcmp(pv,"0")!=0,mask,
                                 [](const char* k){return std::getenv(k);},[](const char* k,const char* v){setenv(k,v,1);});
    sQualityCensus=r.text;
    return true;
}
}
bool ApplyQualitySet(const Config& config) {
    namespace qs=port::qualityset;
    return ApplyQualitySetMode(config.performance60&&config.fpsLimit==0?qs::Mode::Performance:(config.fpsLimit==30||config.fpsLimit==kSteady30)?qs::Mode::Locked30:qs::Mode::Quality);
}
bool ApplyQualitySetFromEnv() {
    namespace qs=port::qualityset;
    const char* t=std::getenv("GXV_AUTOQ_TARGET");const char* f=std::getenv("GXV_FPS_LIMIT");
    return ApplyQualitySetMode(t&&std::atoi(t)==60&&!(f&&std::atoi(f)==30)?qs::Mode::Performance:qs::Mode::Quality);
}
bool ApplyModeKeysFromEnv() {
    if(port::modekeys::Applied())return false;
    port::modekeys::Applied()=true;
    const char* t=std::getenv("GXV_AUTOQ_TARGET");const char* f=std::getenv("GXV_FPS_LIMIT");const char* m=std::getenv("GXV_PICTURE_MODE");
    const bool q=(m&&std::atoi(m)==30)||(f&&std::atoi(f)==30)||(t&&std::atoi(t)==30);
    sModeKeysCensus=port::modekeys::Apply(q,[](const char* k){return std::getenv(k);},[](const char* k,const char* v){setenv(k,v,1);}).text;
    sPerfDefaultsCensus.clear();
    if(!q){char text[512];port::perfdefaults::ApplyPerformance([](const char* k,const char* v){setenv(k,v,1);},text,sizeof text);sPerfDefaultsCensus=text;}   // (pace33 PATCH8: the Performance umbrellas)
    return true;
}
std::vector<Override> ApplyStartup(const Config& menuConfig,bool enabled) {
    std::vector<Override> overrides;if(!enabled)return overrides;
    // 13 Nov: env.txt's GXV_PICTURE_MODE=30 is the WHOLE mode for this launch: the fan-out below, the per-mode key table (port/mode_keys.h) and the Quality umbrella follow it.
    // Before, an env.txt that asked for 30 with the menu on Performance ran a hybrid: the Performance fan-out and the Performance row of the table (cheaper glow, direct display)
    // under a "mode 30" stage table, so a Quality test did not measure Quality. Only 30 can be told from the start-up default (60 is that default's own value). Nothing is saved.
    Config config=menuConfig;
    if(const char* pm=std::getenv("GXV_PICTURE_MODE");pm&&std::atoi(pm)==30&&!Mode30(menuConfig)) {
        config.fpsLimit=30;config.performance60=false;
        overrides.push_back({"GXV_PICTURE_MODE",pm,ChoiceLabel("GXV_PICTURE_MODE",pm)});
    }
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
    ApplyQualitySet(config);   // (gfx26: GXV_QUALITY_SET; nothing happens unless it is set)
    port::modekeys::Applied()=true;
    // (pace33 PATCH8) env.txt's GXV_FPS_LIMIT=30 under the Performance choice asks for the fixed 30 cap, as before: the mode's re-projected 60 (GXV_HRW=5, which owns
    // the pacing and would clear the cap) stands down unless env.txt asked for it too.
    const char* capEnv=std::getenv("GXV_FPS_LIMIT");
    // (setnat31) "env.txt set GXV_HRW" is read from the mode-key snapshot when there is one: on the console entry.cpp sets its own GXV_HRW=0 default before this
    // runs, so a plain getenv was always true there and this rule (and the 60 Assist row below) could never act.
    const bool userCap30=!Is30(config)&&capEnv&&std::atoi(capEnv)==30,userHrw=port::modekeys::UserHas("GXV_HRW",[](const char* k){return std::getenv(k);});
    sModeKeysCensus=port::modekeys::Apply(Is30(config),[](const char* k){return std::getenv(k);},[](const char* k,const char* v){setenv(k,v,1);}).text;   // (deadfix27: the per-mode key table, port/mode_keys.h; env.txt wins)
    sPerfDefaultsCensus.clear();
    if(Is30(config)) {   // (q30-26: the Quality mode's umbrella levels; the stage-knob mode flag)
        char text[640];   // (fmt1131: 320 -> 640; the census line is about 330 bytes with its numbers)
        port::quality30::Applied()=false;   // (one Settings start-up a process; a test may call it again)
        if(port::quality30::Apply([](const char* k){return std::getenv(k);},[](const char* k,const char* v){setenv(k,v,1);},text,sizeof text))sQuality30Census=text;
    } else {   // (a mode change relaunches the game; this keeps a second call in one process honest)
        port::stageknob::SetQuality(false);port::quality30::Applied()=false;sQuality30Census.clear();
        if(!port::modekeys::UserHas("GXV_Q30_MSAA",[](const char* k){return std::getenv(k);}))unsetenv("GXV_Q30_MSAA");
        char text[512];   // (pace33 PATCH8: the Performance mode's umbrella defaults, GXV_CPULOD=2 / GXV_ACROSS=1, for the keys env.txt did not write; port/perf_defaults.h)
        port::perfdefaults::ApplyPerformance([](const char* k,const char* v){setenv(k,v,1);},text,sizeof text);
        sPerfDefaultsCensus=text;
        if(userCap30&&!userHrw){setenv("GXV_HRW","0",1);sPerfDefaultsCensus+="; GXV_HRW = 0 (env.txt GXV_FPS_LIMIT=30: the fixed 30 cap keeps the pacing)";}
        // (setnat31) Settings "60 Assist" Off: only real pictures, the plain 60 target (GXV_HRW=0) instead of the table's estimated second picture (GXV_HRW=5).
        // On (the default) changes nothing: c181's Performance. env.txt's GXV_HRW wins either way. Read here once: it applies from the next start.
        else if(!config.assist60&&!userHrw){setenv("GXV_HRW","0",1);sPerfDefaultsCensus+="; GXV_HRW = 0 (Settings: 60 Assist off)";}
        else if(!config.assist60)sPerfDefaultsCensus+="; 60 Assist off in Settings, but env.txt sets GXV_HRW (env.txt wins)";
        // (Wave 4) Config::allow50 is retired from UI/startup so Performance stays cleanly 60/30;
        // GXV_HRW_ALLOW50=1 in env.txt remains intact in vgx_galaxy.cpp / hrw_mp.h as the developer escape hatch.
    }
    // The 30 mode runs under the fixed 30 cap (when the driver has it; otherwise the game logs that it keeps its old pacing); the 60 mode leaves GXV_FPS_LIMIT unset (no cap = GXV_FPS_LIMIT=0).
    if(!std::getenv("GXV_FPS_LIMIT")&&Is30(config))setenv("GXV_FPS_LIMIT","30",0);
    // (Locked 30 is the quality mode: it starts at the screen's own 960x544 and keeps its size across fades and loads; see
    // vgx_backend_gl.cpp, GXV_AUTOQ_START and GXV_AUTOQ_WIPE_LOCKED30. An overload still steps it down.)
    // Steady 30, the aggressive opt-in mode: the fixed 30 FPS ceiling (never the 30/20 adaptive cadence),
    // dynamic quality aimed at 30 that lowers resolution only as a last resort; effects stay full size. 60 Hz game
    // turns are unchanged. env.txt values win over each part.
    if(config.fpsLimit==kSteady30&&!std::getenv("GXV_FPS_LIMIT")) {setenv("GXV_FPS_LIMIT","30",0);setenv("GXV_AUTOQ_TARGET","30",0);} // full-size effects: 30 is held by speed, not by smaller effects (GXV_FXHALF stays a test switch)
    // (modes26: the Dynamic resolution row is hidden; its saved value is kept in the file but no longer sets GXV_AUTOQ)
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
    // (warp32 PATCH7) GXV_HRW=5 owns the pacing (port/frame_pace.h Hrw5Pacing): the switches the other readers see are made to agree, whoever set them
    // (the menu's 30 mode or env.txt), so that no rule that looks at GXV_FPS_LIMIT / GXV_SKIP_PATTERN decides for a cap that is not there.
    if(port::pace::Hrw5Pacing()) {
        const char* f=std::getenv("GXV_FPS_LIMIT");
        if(f&&std::atoi(f)==30)setenv("GXV_FPS_LIMIT","0",1);
        const char* pc=std::getenv("GXV_PACE");
        if(pc&&std::atoi(pc)!=0)setenv("GXV_PACE","0",1);
        const char* sp=std::getenv("GXV_SKIP_PATTERN");
        if(!sp||std::atoi(sp)==0)setenv("GXV_SKIP_PATTERN","2",1);
        setenv("GXV_PRESENT_SYNC","2",1);
    }
    return overrides;
}
}
