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
    std::cout<<"launch: exact dispatch, Play precedence, regional catalogs, gates and overrides passed\n";
}
