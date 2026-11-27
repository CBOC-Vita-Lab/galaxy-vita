// Compile the actual ripper, including its streamed Yaz0 decoder, with a counted disc.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#define TARGET_PC 1
using u8 = uint8_t; using u32 = uint32_t; using s32 = int32_t;
struct DVDFileInfo { u32 length; };
struct OSMutex { bool held; };
static bool busy, oom, cancel, retryOnce;
static int submissions, lookups, writes, delays;
static void* published;
static u32 publishedSize;
static std::vector<u8> disc;
static void OSInitMutex(OSMutex* m) { m->held=false; }
static void OSLockMutex(OSMutex* m) { assert(!m->held); m->held=true; }
static int OSTryLockMutex(OSMutex* m) { if(busy||m->held)return 0;m->held=true;return 1; }
static void OSUnlockMutex(OSMutex* m) { assert(m->held);m->held=false; }
static void VIWaitForRetrace() { ++delays; }
static void DCInvalidateRange(void*,u32) {}
static void DCStoreRangeNoSync(void* p,u32 bytes) { ++writes;published=p;publishedSize=bytes; }
static void OSReport(const char*,...) {}
static void OSPanic(const char*,int,const char*) { std::abort(); }
static void port_log_summary(const char*,...) {}
static int64_t port_host_time_ns() { return 0; }
static void (*port_load_note_decomp)(u32,int64_t) = nullptr;
static s32 DVDConvertPathToEntrynum(const char*) { ++lookups;return 0; }
static bool DVDFastOpen(s32,DVDFileInfo* f) { f->length=disc.size();return true; }
static s32 DVDReadPrio(DVDFileInfo*,u8* dst,u32 size,u32 off,int) {
    ++submissions;if(cancel)return -3;if(retryOnce){retryOnce=false;return -1;}
    u32 n=off>=disc.size()?0:std::min<u32>(disc.size()-off,size);
    if(n)std::memcpy(dst,disc.data()+off,n);
    std::memset(dst+n,0,size-n);return size;
}
class JKRHeap { public:
    static void* alloc(u32 n,int,JKRHeap*) {return oom?nullptr:std::malloc(n);}
    static void free(void* p,void*) {std::free(p);}
};
void* operator new[](size_t n,JKRHeap*,int) {return std::malloc(n);}
namespace MR { static void copyMemory(void* d,const void* s,u32 n) {std::memmove(d,s,n);} }
class FileRipper { public:
    enum AllocDirection {Head,Tail};static void setup(u32,JKRHeap*);static s32 checkCompressed(const u8*);
    static void* loadToMainRAM(const char*,u8*,bool,JKRHeap*,AllocDirection);
    static bool decompressFromDVD(DVDFileInfo*,void*,u32,u32,const u8*,u32);
    static bool decompressSzsSub(u8*,u8*);static u8* readSrcDataFirst();static u8* readSrcDataNext(u8*);
};
#include "ripper.inc"
static void* load(u8* dst=nullptr,bool decompress=true) {
    submissions=lookups=writes=delays=0;
    auto p=FileRipper::loadToMainRAM("test.arc",dst,decompress,nullptr,FileRipper::Tail);
    assert(lookups==1 && !sDecompMutex.held);return p;
}
static void raw(const std::vector<u8>& data,int expected=-1) {
    disc=data;
    const size_t aligned=(data.size()+31)&~size_t(31);
    std::vector<u8> result(aligned+1,0xab);
    assert(load(result.data())==result.data() && result.back()==0xab);
    assert(!std::memcmp(result.data(),data.data(),data.size()));
    assert(std::all_of(result.begin()+data.size(),result.end()-1,[](u8 v){return !v;}));
    if(submissions==1)assert(writes==1 && published==result.data() && publishedSize==aligned);
    if(expected>=0)assert(submissions==expected);
    auto p=load();assert(p && !std::memcmp(result.data(),p,aligned));std::free(p);
}
int main(int argc,char** argv) {
    FileRipper::setup(0x20000,nullptr);
    const bool off=std::getenv("GXV_ARCHIVE_BATCH") && *std::getenv("GXV_ARCHIVE_BATCH")=='0';
    for(size_t n:{1,31,32,33,32767,32768,32769,131073}) {
        std::vector<u8> v(n,0x55);raw(v,n<=32||(!off&&n<=32768)?1:2);
    }
    busy=true;raw(std::vector<u8>(137,7),2);busy=false;
    disc.resize(4096);cancel=true;assert(!load());cancel=false;
    retryOnce=true;auto p=load();assert(p && submissions==(off?3:2) && delays==1);std::free(p);
    oom=true;assert(!load());oom=false;disc.clear();assert(!load());
    disc.assign(25,0);std::memcpy(disc.data(),"Yaz0",4);disc[7]=8;disc[16]=0xff;std::memcpy(&disc[17],"12345678",8);
    for(bool unavailable:{false,true}) {
        busy=unavailable;p=load();assert(p && !std::memcmp(p,"12345678",8));std::free(p);
    }
    busy=false;disc[2]='y';assert(!load());p=load(nullptr,false);assert(p && !std::memcmp(p,"Yay0",4));std::free(p);
    unsigned count=0, reads=0, copied=0;
    for(int a=1;a<argc;++a) {
        std::ifstream f(argv[a],std::ios::binary);assert(f);
        std::vector<u8> bytes((std::istreambuf_iterator<char>(f)),{});assert(bytes.size()>=4);
        if(!std::memcmp(bytes.data(),"RARC",4)||!std::memcmp(bytes.data(),"CRAR",4)){
            raw(bytes);++count;reads+=submissions;if(submissions==1)copied+=(bytes.size()+31)&~size_t(31);
        }
    }
    std::cout<<"archive output, padding, retries, contention, allocation failures and compression PASS; cooked raw files "
             <<count<<", old/new worker requests "<<count*2<<"/"<<reads<<", bytes copied "<<copied<<"\n";
    std::free(sReadBuffer);
}
