#include "port/settings_launch.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
using namespace port::settings;
static std::vector<std::string> languages;
static std::string selected;
extern "C" int port_language_count() {return static_cast<int>(languages.size());}
extern "C" const char* port_language_name(int i) {return languages.at(static_cast<size_t>(i)).c_str();}
extern "C" void port_language_set(const char* name) {selected=name?name:"";}
int main() {
    auto parse=[](const char* s) {return ParseLaunch(0,nullptr,s,std::char_traits<char>::length(s)+1);};
    for(const auto* s:{"-settings","psla:-settings"," \t-settings \r\n","psla: \"-settings\""}) assert(parse(s)==LaunchRequest::Settings);
    for(const auto* s:{"x-settings","foo=-settings","--settings","xpsla:-settings","-settings-other"}) assert(parse(s)==LaunchRequest::Play);
    for(const auto* s:{"\"-settings","'bad","psla:\"bad"}) assert(parse(s)==LaunchRequest::Malformed);
    const char* argv[]={"-settings"}; assert(ParseLaunch(1,argv,nullptr,0)==LaunchRequest::Settings);
    const char* play[]={"eboot","-play"};assert(ParseLaunch(2,play,"psla:-settings",15)==LaunchRequest::Play);
    const char* stale[]={"-play"};assert(ParseLaunch(1,stale,"unterminated",3)==LaunchRequest::Play);
    assert(ParseLaunch(0,nullptr,"-settings",9)==LaunchRequest::Malformed);
    std::string giant(1025,'x');assert(ParseLaunch(0,nullptr,giant.c_str(),giant.size()+1)==LaunchRequest::Malformed);
    for(const auto& names:std::vector<std::vector<std::string>>{{"English","French","German","Spanish","Italian"},{"English","French","Spanish"},{"Japanese"},{"Korean"},{"English"},{}}) {
        languages=names;assert(ReadLanguages().names==names);
    }
    const char* keys[]={"GXV_FPS_LIMIT","GXV_AUTOQ","GXV_AUTOQ_LEVEL","GXV_AUTOQ_CYCLE","PETARI_LANGUAGE","GXV_REMAP"};
    for(auto key:keys)unsetenv(key);
    auto c=DefaultConfig(port::controls::GetControlApi());c.fpsLimit=30;c.autoQuality=AutoQuality::Enabled;c.language="French";
    assert(ApplyStartup(c,false).empty() && !getenv("GXV_FPS_LIMIT") && selected.empty());
    languages={"English","French"};
    assert(ApplyStartup(c,true).empty());assert(std::string(getenv("GXV_FPS_LIMIT"))=="30");assert(std::string(getenv("GXV_AUTOQ"))=="1");assert(selected=="French");
    for(auto key:keys)setenv(key,"developer",1);
    auto overrides=ApplyStartup(c,true);assert(overrides.size()==6);
    for(auto key:keys)assert(std::string(getenv(key))=="developer");
    for(auto key:keys)unsetenv(key);
    selected="";c.language="Italian";ApplyStartup(c,true);assert(selected.empty());
    for(auto key:keys)unsetenv(key);
    // (Picture mode Quality, the default, runs under the fixed 30 cap: GXV_FPS_LIMIT=30, as Locked 30 does)
    auto clearMode=[]{for(auto k:{"GXV_FPS_LIMIT","GXV_AUTOQ_TARGET","GXV_AUTOQ_EDGESTEP","GXV_AUTOQ_LOCKED30_FLOOR","GXV_AUTOQ_LOCKED30_START","GXV_AUTOQ_MAX","GXV_AUTOQ_MSAA","GXV_FXHALF"})unsetenv(k);};
    clearMode();c.autoQuality=AutoQuality::Default;c.fpsLimit=0;ApplyStartup(c,true);assert(!getenv("GXV_AUTOQ")&&std::string(getenv("GXV_FPS_LIMIT"))=="30");
    assert(!getenv("GXV_PACE")&&!getenv("GXV_AUTOQ_TARGET")&&!getenv("GXV_FXHALF"));clearMode();c.fpsLimit=kSteady30;ApplyStartup(c,true);
    assert(!getenv("GXV_PACE")&&std::string(getenv("GXV_FPS_LIMIT"))=="30"&&std::string(getenv("GXV_AUTOQ_TARGET"))=="30"&&!getenv("GXV_FXHALF"));
    clearMode();
    // The start-up default GXV_AUTOQ_TARGET=30 (the 30 FPS floor) is not reported as an override; another value is (but 60 agrees with Performance).
    setenv("GXV_AUTOQ_TARGET","30",1);c.fpsLimit=0;assert(ApplyStartup(c,true).empty());
    clearMode();setenv("GXV_AUTOQ_TARGET","60",1);assert(ApplyStartup(c,true).size()==1);
    clearMode();setenv("GXV_AUTOQ_TARGET","60",1);c.performance60=true;assert(ApplyStartup(c,true).empty()&&!getenv("GXV_FPS_LIMIT"));c.performance60=false;clearMode();
    // (Performance mode below: it leaves GXV_FPS_LIMIT unset, and what it sets agrees with it, so these repeated start-ups in one process do
    // not see Quality's own cap from the call before as env.txt's)
    c.performance60=true;
    // Reduced motion: the setting sets GXV_REDUCED_MOTION=1 (env.txt wins); only an env value that disagrees with the setting is an override.
    unsetenv("GXV_REDUCED_MOTION");c.reducedMotion=false;assert(ApplyStartup(c,true).empty()&&!getenv("GXV_REDUCED_MOTION"));
    c.reducedMotion=true;assert(ApplyStartup(c,true).empty()&&std::string(getenv("GXV_REDUCED_MOTION"))=="1");
    assert(ApplyStartup(c,false).empty());unsetenv("GXV_REDUCED_MOTION");assert(ApplyStartup(c,false).empty()&&!getenv("GXV_REDUCED_MOTION"));  // disabled start-up touches nothing
    setenv("GXV_REDUCED_MOTION","1",1);assert(ApplyStartup(c,true).empty()&&std::string(getenv("GXV_REDUCED_MOTION"))=="1");   // agrees: not an override
    setenv("GXV_REDUCED_MOTION","0",1);{auto o=ApplyStartup(c,true);assert(o.size()==1&&o[0].key=="GXV_REDUCED_MOTION"&&o[0].effectiveLabel=="Reduced motion off");}
    assert(std::string(getenv("GXV_REDUCED_MOTION"))=="0");                                                                  // env.txt value kept (setenv overwrite 0)
    c.reducedMotion=false;setenv("GXV_REDUCED_MOTION","1",1);{auto o=ApplyStartup(c,true);assert(o.size()==1&&o[0].rawValue=="1"&&o[0].effectiveLabel=="Reduced motion on");}
    setenv("GXV_REDUCED_MOTION","0",1);assert(ApplyStartup(c,true).empty());                                                 // off and off agree
    setenv("GXV_REDUCED_MOTION","",1);assert(ApplyStartup(c,true).empty());                                                  // empty reads as off
    c.reducedMotion=true;{auto o=ApplyStartup(c,true);assert(o.size()==1&&std::string(getenv("GXV_REDUCED_MOTION")).empty());} // empty is off: disagrees, kept
    unsetenv("GXV_REDUCED_MOTION");c.reducedMotion=false;
    c.performance60=false;clearMode();
    // Picture and speed choices (Display page). Each maps to a start-up switch; the game's own defaults set nothing; env.txt wins and a
    // value that disagrees with the menu is reported as a developer override.
    {
        const char* all[]={"GXV_AUTOQ_MAX","GXV_AUTOQ_MSAA","GXV_SHARPEN","GXV_HDTEX","GXV_AUTOQ_FXSTEP","GXV_CHARAA_AUTO","GXV_DOF","GXV_CORE3_IO","GXV_CORE3","GXV_TILT_GAIN","GXV_TILT_STICK","GXV_AUTOQ","GXV_FPS_LIMIT","GXV_REDUCED_MOTION",
                         "GXV_GC_CORE","GXV_AUDIO_THREAD_CORE","GXV_GXM_DIRECT","GXV_DRAWDONE_DEFER","GXV_DL_LAZY","GXV_ANIM_LOD","GXV_ANIM_LOD_DIST","GXV_SLIP","PETARI_FRAMESKIP",
                         "GXV_AUTOQ_TARGET","GXV_AUTOQ_EDGESTEP","GXV_AUTOQ_LOCKED30_FLOOR","GXV_AUTOQ_LOCKED30_START"};
        auto clear=[&]{for(auto k:all)unsetenv(k);};
        auto startup=[&]{setenv("GXV_AUTOQ_TARGET","30",1);setenv("GXV_AUTOQ_FXSTEP","3",1);setenv("GXV_CHARAA_AUTO","1",1);setenv("GXV_CORE3_IO","1",1);
                           setenv("GXV_AUDIO_THREAD_CORE","1",1);setenv("GXV_GXM_DIRECT","1",1);setenv("GXV_DRAWDONE_DEFER","2",1);setenv("GXV_SLIP","1",1);};  // what vita/vgx-game/entry.cpp sets before Settings runs
        auto env=[&](const char* k)->std::string{const char* v=getenv(k);return v?v:"<unset>";};
        auto base=DefaultConfig(port::controls::GetControlApi());
        // Best picture (the defaults): nothing is set and nothing is an override, but Picture mode Quality: the fixed 30 cap, starting at
        // 960x544 (rung 6), the picture size giving way to the game's standard size (rung 2) at most; the 30 target of the start-up.
        clear();startup();assert(ApplyStartup(base,true).empty());
        for(auto k:{"GXV_AUTOQ_MAX","GXV_AUTOQ_MSAA","GXV_SHARPEN","GXV_HDTEX","GXV_DOF","GXV_TILT_GAIN","GXV_TILT_STICK","GXV_GC_CORE","GXV_DL_LAZY","GXV_ANIM_LOD","GXV_ANIM_LOD_DIST","GXV_CORE3"})assert(!getenv(k));
        // (Quality: the present's edge smoothing is the first thing a GPU miss gives up, at 960x544 too: GXV_AUTOQ_HOLD30's edge rule)
        assert(env("GXV_AUTOQ_EDGESTEP")=="1");
        assert(env("GXV_FPS_LIMIT")=="30"&&env("GXV_AUTOQ_LOCKED30_FLOOR")=="0"&&env("GXV_AUTOQ_LOCKED30_START")=="6"&&env("GXV_AUTOQ_TARGET")=="30");
        assert(env("GXV_SLIP")=="0"&&env("PETARI_FRAMESKIP")=="5");   // Game speed Full (the default)
        assert(env("GXV_AUDIO_THREAD_CORE")=="1"&&env("GXV_GXM_DIRECT")=="1"&&env("GXV_DRAWDONE_DEFER")=="4");   // Logic overlap Max (the default)
        assert(env("GXV_AUTOQ_FXSTEP")=="3"&&env("GXV_CHARAA_AUTO")=="1"&&env("GXV_CORE3_IO")=="1");
        // Each choice on its own.
        struct One {const char* what;void (*edit)(Config&);const char* key;const char* value;};
        const One ones[]={
            {"sharper off",[](Config& c){c.aboveNative=false;},"GXV_AUTOQ_MAX","2"},
            {"aa off",[](Config& c){c.antiAliasing=AntiAliasing::Off;},"GXV_AUTOQ_MSAA","0"},
            {"sharpness off",[](Config& c){c.sharpness=Sharpness::Off;},"GXV_SHARPEN","off"},
            {"sharpness strong",[](Config& c){c.sharpness=Sharpness::Strong;},"GXV_SHARPEN","0.8"},
            {"hd off",[](Config& c){c.hdTextures=false;},"GXV_HDTEX","0"},
            {"dof off",[](Config& c){c.depthOfField=false;},"GXV_DOF","0"},
            {"core off",[](Config& c){c.coreHelp=false;},"GXV_CORE3_IO","0"},{"core off: fourth core",[](Config& c){c.coreHelp=false;},"GXV_CORE3","0"},
            {"core off: collector",[](Config& c){c.coreHelp=false;},"GXV_GC_CORE","2"},{"core off: audio",[](Config& c){c.coreHelp=false;},"GXV_AUDIO_THREAD_CORE","0"},
            {"standard renderer",[](Config& c){c.gxmDirect=false;},"GXV_GXM_DIRECT","0"},{"overlap off",[](Config& c){c.logicOverlap=false;},"GXV_DRAWDONE_DEFER","1"},{"overlap on",[](Config& c){c.overlapMax=false;},"GXV_DRAWDONE_DEFER","2"},
            {"materials every turn",[](Config& c){c.lazyMaterials=false;},"GXV_DL_LAZY","0"},{"full far animation",[](Config& c){c.farAnimation=FarAnimation::Full;},"GXV_ANIM_LOD","0"},{"aggressive far animation",[](Config& c){c.farAnimation=FarAnimation::Aggressive;},"GXV_ANIM_LOD_DIST","1000"},
            {"smooth: slip",[](Config& c){c.fullSpeed=false;},"GXV_SLIP","1"},{"smooth: skips",[](Config& c){c.fullSpeed=false;},"PETARI_FRAMESKIP","<unset>"},
            {"effects full",[](Config& c){c.effects=EffectsQuality::Full;},"GXV_AUTOQ_FXSTEP","0"},
            {"mario off",[](Config& c){c.prioritizeMario=false;},"GXV_CHARAA_AUTO","0"},
            {"tilt 1",[](Config& c){c.tiltLevel=1;},"GXV_TILT_GAIN","50"},{"tilt 2",[](Config& c){c.tiltLevel=2;},"GXV_TILT_GAIN","63"},{"tilt 3",[](Config& c){c.tiltLevel=3;},"GXV_TILT_GAIN","75"},
            {"tilt 4",[](Config& c){c.tiltLevel=4;},"GXV_TILT_GAIN","88"},{"tilt 6",[](Config& c){c.tiltLevel=6;},"GXV_TILT_GAIN","120"},{"tilt 7",[](Config& c){c.tiltLevel=7;},"GXV_TILT_GAIN","140"},
            {"tilt 8",[](Config& c){c.tiltLevel=8;},"GXV_TILT_GAIN","160"},{"tilt 9",[](Config& c){c.tiltLevel=9;},"GXV_TILT_GAIN","180"},{"tilt 10",[](Config& c){c.tiltLevel=10;},"GXV_TILT_GAIN","200"},
            {"tilt stick",[](Config& c){c.tiltStick=true;},"GXV_TILT_STICK","1"}};
        for(const auto& one:ones) {
            clear();startup();auto c=base;one.edit(c);
            assert(ApplyStartup(c,true).empty());assert(env(one.key)==one.value);
            assert(ApplyStartup(c,false).empty());   // (a disabled start-up does nothing; the values above are from the enabled call)
        }
        // The fourth core (GXV_CORE3): "auto" (or unset) agrees with Core plan Auto; 1 (every part) is a developer override; 0 agrees with Off.
        {clear();startup();setenv("GXV_CORE3","auto",1);assert(ApplyStartup(base,true).empty());}
        {clear();startup();setenv("GXV_CORE3","1",1);auto o=ApplyStartup(base,true);assert(o.size()==1&&o[0].key=="GXV_CORE3"&&o[0].effectiveLabel=="Fourth core: every part, when available"&&env("GXV_CORE3")=="1");}
        {clear();startup();setenv("GXV_CORE3","0",1);auto c=base;c.coreHelp=false;assert(ApplyStartup(c,true).empty()&&env("GXV_CORE3")=="0");}
        {clear();startup();setenv("GXV_CORE3","0",1);auto o=ApplyStartup(base,true);assert(o.size()==1&&o[0].effectiveLabel=="Fourth core off");}
        // Effects Full keeps Mario's edge smoothing; Mario off ends the whole Mario-first ladder (steps and smoothing).
        {clear();startup();auto c=base;c.effects=EffectsQuality::Full;ApplyStartup(c,true);assert(env("GXV_CHARAA_AUTO")=="1"&&env("GXV_AUTOQ_FXSTEP")=="0");}
        {clear();startup();auto c=base;c.prioritizeMario=false;ApplyStartup(c,true);assert(env("GXV_CHARAA_AUTO")=="0"&&env("GXV_AUTOQ_FXSTEP")=="0");}
        // The presets: Balanced caps at the game's own rung, Performance adds no multisampling, no sharpening and no depth of field.
        {clear();startup();auto c=base;ApplyPreset(c,Preset::Balanced);assert(ApplyStartup(c,true).empty()&&env("GXV_AUTOQ_MAX")=="2"&&env("GXV_AUTOQ_MSAA")=="<unset>"&&env("GXV_SHARPEN")=="<unset>");}
        {clear();startup();auto c=base;ApplyPreset(c,Preset::Performance);assert(ApplyStartup(c,true).empty()&&env("GXV_AUTOQ_MAX")=="2"&&env("GXV_AUTOQ_MSAA")=="0"&&env("GXV_SHARPEN")=="off"&&env("GXV_DOF")=="0"&&env("GXV_AUTOQ_FXSTEP")=="3");
         assert(env("GXV_AUTOQ_TARGET")=="60"&&env("GXV_AUTOQ_EDGESTEP")=="1"&&env("GXV_FPS_LIMIT")=="<unset>");}   // (the Performance preset is Performance mode)
        // Picture mode. Performance: the 60 target, sizes up to 960x544 with Sharper on (rung 6) but no multisampling, edge smoothing as the
        // first step, no fixed cap; Mario and HD textures stay their own rows. Locked 30: the cap and the backend's own 960x544 floor and start.
        {clear();startup();auto c=base;c.performance60=true;assert(ApplyStartup(c,true).empty());
         assert(env("GXV_AUTOQ_TARGET")=="60"&&env("GXV_AUTOQ_MAX")=="6"&&env("GXV_AUTOQ_MSAA")=="0"&&env("GXV_AUTOQ_EDGESTEP")=="1"&&env("GXV_FPS_LIMIT")=="<unset>");
         assert(env("GXV_AUTOQ_LOCKED30_FLOOR")=="<unset>"&&env("GXV_AUTOQ_LOCKED30_START")=="<unset>"&&env("GXV_HDTEX")=="<unset>"&&env("GXV_AUTOQ_FXSTEP")=="3"&&env("GXV_CHARAA_AUTO")=="1");}
        {clear();startup();auto c=base;c.performance60=true;c.aboveNative=false;c.hdTextures=false;c.prioritizeMario=false;ApplyStartup(c,true);
         assert(env("GXV_AUTOQ_MAX")=="2"&&env("GXV_HDTEX")=="0"&&env("GXV_CHARAA_AUTO")=="0"&&env("GXV_AUTOQ_TARGET")=="60");}
        {clear();startup();auto c=base;c.fpsLimit=30;assert(ApplyStartup(c,true).empty());
         assert(env("GXV_FPS_LIMIT")=="30"&&env("GXV_AUTOQ_TARGET")=="30"&&env("GXV_AUTOQ_LOCKED30_FLOOR")=="<unset>"&&env("GXV_AUTOQ_LOCKED30_START")=="<unset>"&&env("GXV_AUTOQ_MAX")=="<unset>"&&env("GXV_AUTOQ_EDGESTEP")=="<unset>");}
        {clear();startup();auto c=base;c.fpsLimit=kSteady30;assert(ApplyStartup(c,true).empty()&&env("GXV_AUTOQ_TARGET")=="30"&&env("GXV_FPS_LIMIT")=="30");}
        // Quality with Sharper off: the floor and the start clamp to the game's standard size in the back end (the menu still asks for 2 and 6).
        {clear();startup();auto c=base;c.aboveNative=false;ApplyStartup(c,true);assert(env("GXV_AUTOQ_MAX")=="2"&&env("GXV_AUTOQ_LOCKED30_FLOOR")=="0"&&env("GXV_FPS_LIMIT")=="30");}
        // env.txt's GXV_FPS_LIMIT wins over Quality's cap and is reported.
        {clear();startup();setenv("GXV_FPS_LIMIT","0",1);auto o=ApplyStartup(base,true);assert(o.size()==1&&o[0].key=="GXV_FPS_LIMIT"&&env("GXV_FPS_LIMIT")=="0");}
        // Values that came from env.txt win. Agreeing values are not overrides, disagreeing ones are, and are kept.
        struct Env {const char* key;const char* value;void (*edit)(Config&);bool override;};
        const Env envs[]={
            {"GXV_AUTOQ_MAX","2",[](Config& c){c.aboveNative=false;},false},{"GXV_AUTOQ_MAX","4",[](Config& c){c.aboveNative=false;},true},
            {"GXV_AUTOQ_MAX","8",[](Config&){},false},{"GXV_AUTOQ_MAX","8",[](Config& c){c.aboveNative=false;},true},{"GXV_AUTOQ_MAX","6",[](Config&){},true},
            {"GXV_AUTOQ_MSAA","0",[](Config& c){c.antiAliasing=AntiAliasing::Off;},false},{"GXV_AUTOQ_MSAA","0",[](Config&){},true},{"GXV_AUTOQ_MSAA","1",[](Config&){},false},
            {"GXV_SHARPEN","auto",[](Config&){},false},{"GXV_SHARPEN","off",[](Config& c){c.sharpness=Sharpness::Off;},false},{"GXV_SHARPEN","0.5",[](Config& c){c.sharpness=Sharpness::Strong;},true},
            {"GXV_SHARPEN","0.8",[](Config& c){c.sharpness=Sharpness::Strong;},false},{"GXV_SHARPEN","off",[](Config&){},true},
            {"GXV_HDTEX","1",[](Config&){},false},{"GXV_HDTEX","0",[](Config&){},true},{"GXV_HDTEX","0",[](Config& c){c.hdTextures=false;},false},{"GXV_HDTEX","2",[](Config&){},true},
            {"GXV_DOF","1",[](Config&){},false},{"GXV_DOF","0",[](Config&){},true},{"GXV_DOF","0",[](Config& c){c.depthOfField=false;},false},
            {"GXV_AUTOQ_FXSTEP","1",[](Config&){},true},{"GXV_AUTOQ_FXSTEP","2",[](Config&){},true},{"GXV_AUTOQ_FXSTEP","3",[](Config&){},false},{"GXV_AUTOQ_FXSTEP","0",[](Config& c){c.effects=EffectsQuality::Full;},false},{"GXV_AUTOQ_FXSTEP","1",[](Config& c){c.effects=EffectsQuality::Full;},true},
            {"GXV_CHARAA_AUTO","0",[](Config&){},true},{"GXV_CHARAA_AUTO","2",[](Config&){},true},{"GXV_CHARAA_AUTO","1",[](Config&){},false},{"GXV_CHARAA_AUTO","0",[](Config& c){c.prioritizeMario=false;},false},
            {"GXV_CORE3_IO","0",[](Config&){},true},{"GXV_CORE3_IO","0",[](Config& c){c.coreHelp=false;},false},
            {"GXV_GC_CORE","2",[](Config&){},true},{"GXV_GC_CORE","1",[](Config&){},false},{"GXV_GC_CORE","2",[](Config& c){c.coreHelp=false;},false},
            {"GXV_AUDIO_THREAD_CORE","0",[](Config&){},true},{"GXV_AUDIO_THREAD_CORE","0",[](Config& c){c.coreHelp=false;},false},
            {"GXV_GXM_DIRECT","0",[](Config&){},true},{"GXV_GXM_DIRECT","0",[](Config& c){c.gxmDirect=false;},false},
            {"GXV_DRAWDONE_DEFER","1",[](Config&){},true},{"GXV_DRAWDONE_DEFER","0",[](Config&){},true},{"GXV_DRAWDONE_DEFER","4",[](Config&){},false},{"GXV_DRAWDONE_DEFER","3",[](Config&){},true},{"GXV_DRAWDONE_DEFER","2",[](Config& c){c.overlapMax=false;},false},{"GXV_DRAWDONE_DEFER","1",[](Config& c){c.logicOverlap=false;},false},
            {"GXV_DL_LAZY","1",[](Config&){},true},{"GXV_DL_LAZY","2",[](Config&){},false},{"GXV_DL_LAZY","0",[](Config& c){c.lazyMaterials=false;},false},
            {"GXV_ANIM_LOD","0",[](Config&){},true},{"GXV_ANIM_LOD","1",[](Config&){},false},{"GXV_ANIM_LOD","0",[](Config& c){c.farAnimation=FarAnimation::Full;},false},
            {"GXV_SLIP","1",[](Config& c){c.fullSpeed=false;},false},{"PETARI_FRAMESKIP","3",[](Config&){},true},{"PETARI_FRAMESKIP","5",[](Config&){},false},{"PETARI_FRAMESKIP","3",[](Config& c){c.fullSpeed=false;},false},{"GXV_ANIM_LOD_DIST","5000",[](Config&){},true},
            {"GXV_AUTOQ_TARGET","60",[](Config& c){c.performance60=true;},false},{"GXV_AUTOQ_TARGET","60",[](Config&){},true},
            {"GXV_AUTOQ_TARGET","0",[](Config&){},true},{"GXV_AUTOQ_TARGET","45",[](Config& c){c.performance60=true;},true},
            {"GXV_AUTOQ_MAX","6",[](Config& c){c.performance60=true;},false},{"GXV_AUTOQ_MAX","8",[](Config& c){c.performance60=true;},true},{"GXV_AUTOQ_MSAA","0",[](Config& c){c.performance60=true;},false},
            {"GXV_AUTOQ_EDGESTEP","1",[](Config& c){c.performance60=true;},false},{"GXV_AUTOQ_EDGESTEP","0",[](Config& c){c.performance60=true;},true},{"GXV_AUTOQ_EDGESTEP","1",[](Config&){},false},
            {"GXV_AUTOQ_EDGESTEP","0",[](Config&){},true},{"GXV_AUTOQ_EDGESTEP","1",[](Config& c){c.fpsLimit=30;},true},{"GXV_AUTOQ_EDGESTEP","0",[](Config& c){c.fpsLimit=30;},false},
            {"GXV_AUTOQ_LOCKED30_FLOOR","0",[](Config&){},false},{"GXV_AUTOQ_LOCKED30_FLOOR","4",[](Config&){},true},{"GXV_AUTOQ_LOCKED30_FLOOR","6",[](Config& c){c.fpsLimit=30;},false},
            {"GXV_AUTOQ_LOCKED30_FLOOR","2",[](Config& c){c.fpsLimit=30;},true},{"GXV_AUTOQ_LOCKED30_START","6",[](Config&){},false},{"GXV_AUTOQ_LOCKED30_START","7",[](Config&){},true},
            {"GXV_TILT_GAIN","100",[](Config&){},false},{"GXV_TILT_GAIN","140",[](Config& c){c.tiltLevel=7;},false},
            {"GXV_TILT_GAIN","150",[](Config&){},true},{"GXV_TILT_GAIN","100",[](Config& c){c.tiltLevel=1;},true},{"GXV_TILT_GAIN","75",[](Config& c){c.tiltLevel=3;},false},{"GXV_TILT_GAIN","63",[](Config& c){c.tiltLevel=2;},false},{"GXV_TILT_GAIN","140",[](Config& c){c.tiltLevel=6;},true},
            {"GXV_TILT_STICK","1",[](Config& c){c.tiltStick=true;},false},{"GXV_TILT_STICK","1",[](Config&){},true},{"GXV_TILT_STICK","0",[](Config& c){c.tiltStick=true;},true},{"GXV_TILT_STICK","0",[](Config&){},false}};
        for(const auto& e:envs) {
            clear();startup();setenv(e.key,e.value,1);auto c=base;e.edit(c);
            const auto o=ApplyStartup(c,true);
            size_t mine=0;for(const auto& x:o)mine+=x.key==e.key;
            assert((mine==1)==e.override);assert(env(e.key)==e.value);   // never changed
            if(e.override)for(const auto& x:o)if(x.key==e.key){assert(x.rawValue==e.value&&!x.effectiveLabel.empty());}
        }
        clear();startup();
        // The earlier choices still work with the new ones set.
        setenv("GXV_AUTOQ","1",1);assert(ApplyStartup(base,true).size()==1);
        // The tilt choices: the default sets nothing, and the label of a developer override names what env.txt did.
        {clear();startup();auto c=base;c.tiltLevel=10;c.tiltStick=true;setenv("GXV_TILT_GAIN","50",1);setenv("GXV_TILT_STICK","0",1);
         const auto o=ApplyStartup(c,true);assert(o.size()==2&&o[0].key=="GXV_TILT_GAIN"&&o[0].effectiveLabel=="Tilt sensitivity 50%"&&o[1].key=="GXV_TILT_STICK"&&o[1].effectiveLabel=="Tilt from the motion sensor");
         assert(env("GXV_TILT_GAIN")=="50"&&env("GXV_TILT_STICK")=="0");}
        clear();
    }
    std::cout<<"launch: exact dispatch, Play precedence, regional catalogs, gates and overrides passed\n";
}
