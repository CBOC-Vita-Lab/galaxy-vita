#include "port/title_cloud.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

using Bytes = std::vector<uint8_t>;
static void put(Bytes& b, size_t p, unsigned v) {
    for (unsigned k = 0; k < 4; ++k) b.at(p+k) = uint8_t(v >> (k*8));
}
static bool apply(Bytes& b) { return port::titlecloud::BaseCloud(b.data(), b.size()); }
static Bytes read(const char* f) {
    std::ifstream in(f, std::ios::binary);
    assert(in);
    Bytes arc{std::istreambuf_iterator<char>(in), {}};
    const char head[] = "2D3J4ldb";
    auto it = std::search(arc.begin(), arc.end(), head, head+8);
    assert(it != arc.end() && arc.end()-it >= 32);
    const auto len = port::titlecloud::U32(&*it+8);
    assert(len <= size_t(arc.end()-it));
    return {it, it+len};
}
int main(int argc, char** argv) {
    Bytes b(200,0);
    std::memcpy(b.data(),"2D3J4ldb",8);
    put(b,8,b.size());put(b,12,2);std::memcpy(b.data()+32,"PREF",4);put(b,36,8);
    std::memcpy(b.data()+40,"1XET",4);put(b,44,160);put(b,48,2);put(b,52,20);put(b,56,84);
    const size_t h=40+20+32,n=40+84;
    b[n]=2;b[n+6]=12;b[n+10]=20;
    std::memcpy(&b[n+12],"Earth",6);std::memcpy(&b[n+20],"Cloud01k",9);
    b[h]=14;b[h+3]=b[h+5]=b[h+16]=b[h+21]=1;
    b[h+20]=5;b[h+23]=24;b[h+24]=4;b[h+26]=200;
    Bytes expect=b;
    expect[h+16]=expect[h+23]=expect[h+26]=0;
    expect[h+20]=expect[h+24]=1;
    Bytes got=b;
    assert(apply(got) && got==expect && !apply(got));
    for (size_t i=0;i<b.size();++i) {
        Bytes truncated(b.begin(),b.begin()+i);const auto orig=truncated;
        assert(!apply(truncated) && orig==truncated);
    }
    for (size_t i: std::array<size_t,24>{0,4,8,12,36,40,44,48,52,56,n,n+10,n+20,h,h+2,h+4,
                                           h+16,h+20,h+21,h+22,h+23,h+24,h+26,h+27}) {
        Bytes bad=b;bad[i]^=255;const auto orig=bad;
        if(i==12) continue; // a longer block list does not change an already valid first TEX1
        assert(!apply(bad) && orig==bad);
    }
    if(argc==3) {
        Bytes native=read(argv[1]),probed=read(argv[2]);
        assert(native!=probed && apply(native) && native==probed && !apply(native));
        std::printf("Production in-memory BDL sampler equals in-game A/B probe byte-for-byte (%zu bytes).\n",native.size());
    }
    std::puts("title-cloud: exact sampler, bystanders, idempotence, malformed headers and truncations PASS");
}
