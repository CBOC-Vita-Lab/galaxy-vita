// Use production tour dispatch, row selection, layer resolution and JMap reads;
// stand in for the game's stage registry and transition side effects only.
#include "Game/Util/JMapInfo.hpp"
#include "port/tour_scenario.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>

namespace JGadget {
u32 getHashCode(const char* p) { u32 v=0; while (*p) v=v*31+*p++; return v; }
}
namespace MR {
bool isEqualString(const char* a,const char* b) { return !strcmp(a,b); }
bool isEqualStringCase(const char* a,const char* b) { return !strcasecmp(a,b); }
}
extern "C" void port_irq_poll() {}
struct ScenarioData {
    JMapInfo* mScenarioData;
    JMapInfoIter getScenarioDataIter(s32 n) const;
    bool getValueU32(const char*,s32,u32*) const;
    bool getValueString(const char*,s32,const char**) const;
    bool getValueBool(const char*,s32,bool*) const;
};
static ScenarioData* current;
static ScenarioData* getCurrentScenarioData() { return current; }
#include "rows.inc"
namespace ScenarioDataFunction {
#include "layers.inc"
}
struct GalaxyStatusAccessor {
    const ScenarioData* mScenarioData;
    bool isHiddenStar(s32 n) const {
        bool hidden=false;
        assert(mScenarioData->getValueBool("IsHidden",n,&hidden));
        return hidden;
    }
};
struct JMapIdInfo { int _0, mZoneID; JMapIdInfo(int a,int z):_0(a),mZoneID(z){} };
struct GalaxyMoveArgument {
    std::string stage; int placed,_C; JMapIdInfo start;
    GalaxyMoveArgument(int,const char* n,int p,const JMapIdInfo* s):stage(n),placed(p),_C(-1),start(*s){}
};
static int moves, legWrites, startWrites, placed, selected, start, zone;
static bool overrideStart;
static std::string leg, spec, stage;
namespace MR {
GalaxyStatusAccessor makeGalaxyStatusAccessor(const char* n) { return {!strcmp(n,"AstroGalaxy")?current:nullptr}; }
int getPlacedHiddenStarScenarioNo(const char*,int n) { assert(n==4); return 2; }
JMapIdInfo getInitializeStartIdInfo() { return {7,9}; }
}
namespace GameSequenceFunction {
void requestGalaxyMove(const GalaxyMoveArgument& a) {
    ++moves; placed=a.placed;selected=a._C;stage=a.stage; start=a.start._0;zone=a.start.mZoneID;
}
}
static void port_log_summary(const char*,...) {}
static void OSReport(const char*,...) {}
static void portBossLegSet(const char* n,const char* s) { ++legWrites;leg=n?n:"";spec=s?s:""; }
static void portStartOverrideSet(bool value) { ++startWrites;overrideStart=value; }
#include "tour.inc"

int main() {
    // Non-sequential IDs, no trailing readable row, 4-byte scalars at offset 2.
    constexpr unsigned cols=4, rows=3, size=24, header=16+cols*12;
    alignas(4) unsigned char bytes[header+size*rows]{};
    auto* h=reinterpret_cast<JMapData*>(bytes);
    h->mNumEntries=rows;h->mNumFields=cols;h->mDataOffset=header;h->mEntrySize=size;
    auto* c=const_cast<JMapItem*>(h->mItems);
    c[0]={JGadget::getHashCode("ScenarioNo"),~0u,2,0,JMAP_VALUE_TYPE_LONG};
    c[1]={JGadget::getHashCode("Layer"),~0u,6,0,JMAP_VALUE_TYPE_LONG};
    c[2]={JGadget::getHashCode("Label"),~0u,12,0,JMAP_VALUE_TYPE_STRING};
    c[3]={JGadget::getHashCode("IsHidden"),1,20,0,JMAP_VALUE_TYPE_LONG};
    for(unsigned i=0;i<rows;++i) {
        const int ids[]={2,4,1}, layer=3+i,hidden=i==1;
        char* p=reinterpret_cast<char*>(bytes)+header+size*i;
        memcpy(p+2,&ids[i],4);memcpy(p+6,&layer,4);memcpy(p+20,&hidden,4);
        if(i!=1) std::strcpy(p+12,"visible");
    }
    JMapInfo info;info.attach(bytes);ScenarioData sc{&info};
    assert(ScenarioDataFunction::getCurrentScenarioLayers("Layer",1)==0);
    current=&sc;
    assert(ScenarioDataFunction::getCurrentScenarioLayers("Layer",2)==6);
    assert(ScenarioDataFunction::getCurrentScenarioLayers("Layer",3)==0);
    const char* label="untouched";const char* saved=label;bool b=true;u32 v=0x12345678;
    assert(!sc.getValueString("Label",3,&label) && label==saved);
    assert(!sc.getValueBool("IsHidden",3,&b) && b);
    assert(!sc.getValueU32("Layer",99,&v) && v==0x12345678);
    assert(sc.getValueString("Label",4,&label) && label==nullptr);
    assert(sc.getValueString("Label",2,&label) && !strcmp(label,"visible"));
    assert(!sc.getValueString("absent",2,&label) && !strcmp(label,"visible"));
    assert(info.findElement("Layer","bogus",0)==info.end() && info.findElementBinary("Layer","bogus")==info.end());
    assert(portTourEnter("AstroGalaxy:4#12/3@boss+a,other",1));
    assert(stage=="AstroGalaxy" && placed==2 && selected==4 && overrideStart && start==12 && zone==3);
    assert(leg=="AstroGalaxy" && spec=="boss+a");
    for(auto bad: {"","garbage:1@boss","AstroGalaxy:-1@boss","AstroGalaxy:@boss","AstroGalaxy:0","AstroGalaxy:3@bad","AstroGalaxy:4bad"}) {
        assert(!portTourEnter(bad,2));assert(moves==1 && legWrites==1 && startWrites==1);
        assert(leg=="AstroGalaxy" && spec=="boss+a" && overrideStart);
    }
    assert(portTourEnter("AstroGalaxy",3));assert(placed==1 && selected==1 && start==7 && zone==9);
    assert(moves==2 && leg.empty() && spec.empty() && !overrideStart);
    std::puts("PASS: production tour rejects missing stage/row and invalid number without mutating the current leg; valid/default/hidden dispatch and missing/current layer getters preserved.");
}
