#include "port/settings_launch.h"
#include "port/port.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace port::settings {
namespace {
bool Space(char c) {return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\v'||c=='\f';}
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
    for(const auto* key:{"GXV_FPS_LIMIT","GXV_PACE","GXV_AUTOQ_TARGET","GXV_FXHALF","GXV_AUTOQ","GXV_AUTOQ_LEVEL","GXV_AUTOQ_CYCLE","PETARI_LANGUAGE","GXV_REMAP"}) {
        if(const char* raw=std::getenv(key)) {
            // Remapping is on by default (the start-up default sets GXV_REMAP=1); only turning it off is an override.
            if(std::strcmp(key,"GXV_REMAP")==0&&std::strcmp(raw,"1")==0)continue;
            std::string effective=raw;
            if(std::strcmp(key,"GXV_FPS_LIMIT")==0) effective=std::strcmp(raw,"30")==0?"30 FPS requested (driver capability required)":"Existing picture-rate path";
            if(std::strcmp(key,"GXV_REMAP")==0) effective=std::strcmp(raw,"0")==0?"Remapping disabled":"Remapping off (unknown value)";
            overrides.push_back({key,raw,effective});
        }
    }
    if(!std::getenv("GXV_FPS_LIMIT")&&config.fpsLimit==30)setenv("GXV_FPS_LIMIT","30",0);
    // Steady 30, the aggressive opt-in mode: the adaptive 30/20 picture cadence, dynamic quality aimed at
    // 30 and half-size effects (60 Hz game turns unchanged). env.txt values win over each part.
    if(config.fpsLimit==kSteady30&&!std::getenv("GXV_FPS_LIMIT")) {setenv("GXV_PACE","1",0);setenv("GXV_AUTOQ_TARGET","30",0);setenv("GXV_FXHALF","2",0);}
    if(!std::getenv("GXV_AUTOQ")&&!std::getenv("GXV_AUTOQ_LEVEL")&&!std::getenv("GXV_AUTOQ_CYCLE")&&config.autoQuality!=AutoQuality::Default)
        setenv("GXV_AUTOQ",config.autoQuality==AutoQuality::Enabled?"1":"0",0);
    if(!std::getenv("PETARI_LANGUAGE")&&!config.language.empty()) {
        const auto catalog=ReadLanguages();
        // An ordinary launch may not have discovered the disc yet; the existing
        // discovery code validates the wanted canonical name against its catalog.
        if(catalog.names.empty()||std::find(catalog.names.begin(),catalog.names.end(),config.language)!=catalog.names.end())port_language_set(config.language.c_str());
    }
    return overrides;
}
}
