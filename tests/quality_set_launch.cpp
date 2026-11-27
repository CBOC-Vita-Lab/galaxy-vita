// gfx26: GXV_QUALITY_SET through port::settings::ApplyStartup / ApplyQualitySetFromEnv. One mode per process (the umbrella applies once per process):
//   quality_set_launch <quality|locked|performance|env-quality|env-performance>
#include "port/settings_launch.h"
#include "port/quality_set.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
using namespace port::settings;
extern "C" int port_language_count() {return 0;}
extern "C" const char* port_language_name(int) {return "";}
extern "C" void port_language_set(const char*) {}
static std::string get(const char* k) {const char* v=getenv(k);return v?v:"<unset>";}
int main(int argc,char** argv) {
    const std::string m=argc>1?argv[1]:"quality";
    // entry.cpp: the snapshot of env.txt's keys (here: the user set GXV_HUD_NATIVE=0 and nothing else), then its defaults
    setenv("GXV_HUD_NATIVE","0",1);
    port::qualityset::SnapshotUser([](const char* k){return getenv(k);});
    setenv("GXV_EDGE_AA","1",0);setenv("GXV_PLAYER_DOF","1",0);setenv("GXV_ANISO","4",0);
    setenv("GXV_QUALITY_SET","auto",1);
    auto c=DefaultConfig(port::controls::GetControlApi());
    if(m=="quality") {c.fpsLimit=0;c.performance60=false;ApplyStartup(c,true);}
    else if(m=="locked") {c.fpsLimit=30;ApplyStartup(c,true);}
    else if(m=="performance") {c.fpsLimit=0;c.performance60=true;ApplyStartup(c,true);}
    else if(m=="env-quality") {setenv("GXV_AUTOQ_TARGET","30",1);assert(ApplyQualitySetFromEnv());}
    else if(m=="env-performance") {setenv("GXV_AUTOQ_TARGET","60",1);assert(ApplyQualitySetFromEnv());}
    else if(m=="off") {unsetenv("GXV_QUALITY_SET");c.fpsLimit=0;ApplyStartup(c,true);assert(QualitySetCensus().empty()&&get("GXV_PLAYER_DOF")=="1"&&get("GXV_EDGE_DEPTH")=="<unset>");std::cout<<"off ok\n";return 0;}
    const std::string census=QualitySetCensus();
    std::cout<<m<<": "<<census<<"\n";
    assert(!census.empty());
    assert(get("GXV_HUD_NATIVE")=="0");                       // env.txt's own value kept
    assert(get("GXV_PLAYER_DOF")=="0");                       // entry.cpp's default "1" overwritten
    assert(get("GXV_ANISO")=="3");
    assert(census.find("kept from env.txt: GXV_HUD_NATIVE=0")!=std::string::npos);
    if(m=="performance"||m=="env-performance") {
        assert(census.find("(Performance mode")!=std::string::npos);
        assert(get("GXV_EDGE_DEPTH")=="<unset>"&&get("GXV_PLAYER_LODBIAS")=="-25");
        assert(get("GXV_AUTOQ_EDGESTEP")!="0"||getenv("GXV_AUTOQ_EDGESTEP")==nullptr||m=="env-performance");
    } else {
        assert(get("GXV_EDGE_DEPTH")=="1"&&get("GXV_PLAYER_LODBIAS")=="-50"&&get("GXV_AUTOQ_EDGESTEP")=="0"&&get("GXV_AUTOMIP")=="1");
        assert(census.find(m=="locked"?"(Locked 30 mode":"(Quality mode")!=std::string::npos);
    }
    assert(!ApplyQualitySetFromEnv());                        // once per process
    std::cout<<m<<" ok\n";
    return 0;
}
