#include "Game/Util/JMapInfo.hpp"
#include "port/tour_scenario.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <strings.h>
#include <vector>

static unsigned hashes;
namespace JGadget {
u32 getHashCode(const char* s) {
    ++hashes;
    u32 h = 0;
    while (*s) h = h * 31 + *s++;
    return h;
}
}
namespace MR { bool isEqualStringCase(const char* a, const char* b) { return !strcasecmp(a,b); } }
extern "C" void port_irq_poll() {}

static void realTable(const char* path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    assert(f);
    const auto size=static_cast<size_t>(f.tellg());
    assert(size>=16 && size<4u*1024u*1024u);
    std::vector<u32> bytes((size+3)/4);
    f.seekg(0); assert(f.read(reinterpret_cast<char*>(bytes.data()),size));
    JMapInfo info;
    info.attach(bytes.data());
    const int rows=info.getNumEntries(), name=info.searchItemInfo("name");
    assert(rows>0 && rows<256 && name>=0);
    std::vector<const char*> names;
    std::vector<unsigned> order, keys;
    for(int i=0;i<rows;++i) {
        const char* text=nullptr;
        assert(info.getValueFast(i,name,&text));
        names.push_back(text); order.push_back(i);
    }
    names.push_back("unknown-test-stage"); order.push_back(rows);
    names.push_back("another-unknown-stage"); order.push_back(rows+1);
    std::mt19937 rand(183);
    std::shuffle(order.begin(),order.end(),rand);
    auto ref=order;
    auto lookup=[&](unsigned i) {
        auto it=info.findElement("name",names[i],0);
        return it==info.end()?~0u:static_cast<unsigned>(it.mIndex);
    };
    unsigned calls=0;
    std::sort(ref.begin(),ref.end(),[&](unsigned l,unsigned r){calls+=2;return lookup(l)<lookup(r);});
    for(unsigned i=0;i<names.size();++i) keys.push_back(lookup(i));
    std::sort(order.begin(),order.end(),[&](unsigned l,unsigned r){return keys[l]<keys[r];});
    assert(ref==order);
    std::printf("Cooked GalaxyID: %d authored IDs + 2 misses, identical ordered IDs; shuffled test sort %u -> %zu table queries\n",
                rows,calls,keys.size());
}

int main(int argc,char** argv) {
    int32_t scenario=123;
    for(const char* bad: {"","0","-2","2oops"," +1","2147483648","3000000000000"}) {
        assert(!port::TourScenarioNumber(bad,scenario) && scenario==123);
    }
    assert(port::TourScenarioNumber("01",scenario) && scenario==1);
    constexpr int rows=512, cols=12, stride=24, start=16+12*cols;
    std::vector<u32> blob((start + rows*stride)/4);
    auto* hdr = reinterpret_cast<JMapData*>(blob.data());
    hdr->mNumEntries=rows; hdr->mNumFields=cols; hdr->mDataOffset=start; hdr->mEntrySize=stride;
    JMapItem* col = const_cast<JMapItem*>(hdr->mItems);
    col[cols-2]={JGadget::getHashCode("ScenarioNo"),~0u,0,0,JMAP_VALUE_TYPE_LONG};
    col[cols-1]={JGadget::getHashCode("MessageId"),~0u,4,0,JMAP_VALUE_TYPE_STRING};
    for(int i=0;i<rows;++i) {
        auto* row=reinterpret_cast<char*>(hdr)+start+i*stride;
        *reinterpret_cast<s32*>(row)=i/2;
        std::snprintf(row+4,stride-4,"ID%04d",i);
    }
    JMapInfo info;
    info.attach(hdr);
    for (int i=0;i<rows/2;++i) {
        assert(info.findElement("ScenarioNo",i,0).mIndex==2*i);
        assert(info.findElement("ScenarioNo",i,2*i+1).mIndex==2*i+1);
    }
    hashes=0;
    assert(info.findElement("ScenarioNo",999,0)==info.end());
    assert(hashes==1);
    hashes=0;
    assert(info.findElementBinary("MessageId","ID0396").mIndex==396);
    assert(hashes==1);
    assert(info.findElement("ScenarioNo",0,-1)==info.end());
    assert(info.findElement("nonexistent","missing",0)==info.end());
    assert(info.findElement("MessageId",0,0)==info.end());
    assert(info.findElementBinary("missing","ID0256")==info.end());
    assert(info.findElement("ScenarioNo","numeric-is-not-text",0)==info.end());
    assert(info.findElementBinary("ScenarioNo","numeric-is-not-text")==info.end());
    assert(info.findElement("MessageId","ID0511",0).mIndex==511);
    assert(info.findElement("MessageId","ID0511",512)==info.end());
    JMapInfo empty;
    assert(empty.findElementBinary("MessageId","ID0000")==empty.end());
    auto measure=[&](bool fast){
        int sum=0; hashes=0;
        const auto t=std::chrono::steady_clock::now();
        for(int n=0;n<4000;++n) {
            const int wanted=(n*53)%(rows/2);
            if(fast) sum+=info.findElement("ScenarioNo",wanted,0).mIndex;
            else for(int i=0;i<rows;++i) {
                s32 v;
                if(info.getValue(i,"ScenarioNo",&v) && v==wanted){sum+=i;break;}
            }
        }
        const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t).count();
        std::printf("%s 4,000 mixed queries / 512 rows: %u field lookups, %.3f us/query on host\n",fast?"C183":"C182",hashes,us/4000.);
        return sum;
    };
    const int baseline=measure(false);
    assert(measure(true)==baseline);
    if(argc==2) realTable(argv[1]);
}
