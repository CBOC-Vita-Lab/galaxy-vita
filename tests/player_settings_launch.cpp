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
    c.autoQuality=AutoQuality::Default;c.fpsLimit=0;ApplyStartup(c,true);assert(!getenv("GXV_AUTOQ")&&!getenv("GXV_FPS_LIMIT"));
    assert(!getenv("GXV_PACE")&&!getenv("GXV_AUTOQ_TARGET")&&!getenv("GXV_FXHALF"));c.fpsLimit=kSteady30;ApplyStartup(c,true);
    assert(!getenv("GXV_PACE")&&std::string(getenv("GXV_FPS_LIMIT"))=="30"&&std::string(getenv("GXV_AUTOQ_TARGET"))=="30"&&!getenv("GXV_FXHALF"));
    unsetenv("GXV_FPS_LIMIT");unsetenv("GXV_AUTOQ_TARGET");unsetenv("GXV_FXHALF");
    // The start-up default GXV_AUTOQ_TARGET=30 (the 30 FPS floor) is not reported as an override; another value is.
    setenv("GXV_AUTOQ_TARGET","30",1);c.fpsLimit=0;assert(ApplyStartup(c,true).empty());
    setenv("GXV_AUTOQ_TARGET","60",1);assert(ApplyStartup(c,true).size()==1);unsetenv("GXV_AUTOQ_TARGET");
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
    // Picture and speed choices (Display page). Each maps to a start-up switch; the game's own defaults set nothing; env.txt wins and a
    // value that disagrees with the menu is reported as a developer override.
    {
        const char* all[]={"GXV_AUTOQ_MAX","GXV_AUTOQ_MSAA","GXV_SHARPEN","GXV_HDTEX","GXV_AUTOQ_FXSTEP","GXV_CHARAA_AUTO","GXV_DOF","GXV_CORE3_IO","GXV_AUTOQ","GXV_FPS_LIMIT","GXV_REDUCED_MOTION"};
        auto clear=[&]{for(auto k:all)unsetenv(k);};
        auto startup=[&]{setenv("GXV_AUTOQ_FXSTEP","2",1);setenv("GXV_CHARAA_AUTO","2",1);setenv("GXV_CORE3_IO","1",1);};  // what vita/vgx-game/entry.cpp sets before Settings runs
        auto env=[&](const char* k)->std::string{const char* v=getenv(k);return v?v:"<unset>";};
        auto base=DefaultConfig(port::controls::GetControlApi());
        // Best picture (the defaults): nothing is set and nothing is an override.
        clear();startup();assert(ApplyStartup(base,true).empty());
        for(auto k:{"GXV_AUTOQ_MAX","GXV_AUTOQ_MSAA","GXV_SHARPEN","GXV_HDTEX","GXV_DOF"})assert(!getenv(k));
        assert(env("GXV_AUTOQ_FXSTEP")=="2"&&env("GXV_CHARAA_AUTO")=="2"&&env("GXV_CORE3_IO")=="1");
        // Each choice on its own.
        struct One {const char* what;void (*edit)(Config&);const char* key;const char* value;};
        const One ones[]={
            {"sharper off",[](Config& c){c.aboveNative=false;},"GXV_AUTOQ_MAX","2"},
            {"aa off",[](Config& c){c.antiAliasing=AntiAliasing::Off;},"GXV_AUTOQ_MSAA","0"},
            {"sharpness off",[](Config& c){c.sharpness=Sharpness::Off;},"GXV_SHARPEN","off"},
            {"sharpness strong",[](Config& c){c.sharpness=Sharpness::Strong;},"GXV_SHARPEN","0.8"},
            {"hd off",[](Config& c){c.hdTextures=false;},"GXV_HDTEX","0"},
            {"dof off",[](Config& c){c.depthOfField=false;},"GXV_DOF","0"},
            {"core off",[](Config& c){c.coreHelp=false;},"GXV_CORE3_IO","0"},
            {"effects full",[](Config& c){c.effects=EffectsQuality::Full;},"GXV_AUTOQ_FXSTEP","0"},
            {"mario off",[](Config& c){c.prioritizeMario=false;},"GXV_CHARAA_AUTO","0"}};
        for(const auto& one:ones) {
            clear();startup();auto c=base;one.edit(c);
            assert(ApplyStartup(c,true).empty());assert(env(one.key)==one.value);
            assert(ApplyStartup(c,false).empty());   // (a disabled start-up does nothing; the values above are from the enabled call)
        }
        // Effects Full keeps Mario's edge smoothing; Mario off ends the whole Mario-first ladder (steps and smoothing).
        {clear();startup();auto c=base;c.effects=EffectsQuality::Full;ApplyStartup(c,true);assert(env("GXV_CHARAA_AUTO")=="2"&&env("GXV_AUTOQ_FXSTEP")=="0");}
        {clear();startup();auto c=base;c.prioritizeMario=false;ApplyStartup(c,true);assert(env("GXV_CHARAA_AUTO")=="0"&&env("GXV_AUTOQ_FXSTEP")=="0");}
        // The presets: Balanced caps at the game's own rung, Performance adds no multisampling, no sharpening and no depth of field.
        {clear();startup();auto c=base;ApplyPreset(c,Preset::Balanced);assert(ApplyStartup(c,true).empty()&&env("GXV_AUTOQ_MAX")=="2"&&env("GXV_AUTOQ_MSAA")=="<unset>"&&env("GXV_SHARPEN")=="<unset>");}
        {clear();startup();auto c=base;ApplyPreset(c,Preset::Performance);assert(ApplyStartup(c,true).empty()&&env("GXV_AUTOQ_MAX")=="2"&&env("GXV_AUTOQ_MSAA")=="0"&&env("GXV_SHARPEN")=="off"&&env("GXV_DOF")=="0"&&env("GXV_AUTOQ_FXSTEP")=="2");}
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
            {"GXV_AUTOQ_FXSTEP","1",[](Config&){},true},{"GXV_AUTOQ_FXSTEP","0",[](Config& c){c.effects=EffectsQuality::Full;},false},{"GXV_AUTOQ_FXSTEP","1",[](Config& c){c.effects=EffectsQuality::Full;},true},
            {"GXV_CHARAA_AUTO","0",[](Config&){},true},{"GXV_CHARAA_AUTO","1",[](Config&){},true},{"GXV_CHARAA_AUTO","0",[](Config& c){c.prioritizeMario=false;},false},
            {"GXV_CORE3_IO","0",[](Config&){},true},{"GXV_CORE3_IO","0",[](Config& c){c.coreHelp=false;},false}};
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
        clear();unsetenv("GXV_AUTOQ_FXSTEP");unsetenv("GXV_CHARAA_AUTO");unsetenv("GXV_CORE3_IO");
    }
    std::cout<<"launch: exact dispatch, Play precedence, regional catalogs, gates and overrides passed\n";
}
