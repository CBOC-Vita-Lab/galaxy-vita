#pragma once
#include "port/player_settings.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
struct UiFiles {
    std::map<std::string,std::string> data;
    struct Handle {std::string path;size_t at=0;};
    std::map<int64_t,Handle> handles;
    int64_t next=1;
    bool failWrite=false;
    unsigned writes=0;
    static UiFiles& get(void* p) {return *static_cast<UiFiles*>(p);}
    port::settings::FileOps ops() {return {this,
        [](void* p,const char* n)->int64_t {auto& f=get(p);if(!f.data.count(n))return -ENOENT;auto h=f.next++;f.handles[h]={n,0};return h;},
        [](void* p,const char* n)->int64_t {auto& f=get(p);++f.writes;if(f.failWrite)return -EIO;auto h=f.next++;f.handles[h]={n,0};f.data[n]="";return h;},
        [](void* p,int64_t h,void* b,size_t z)->int64_t {auto& f=get(p);auto& file=f.handles.at(h);auto& text=f.data.at(file.path);size_t n=std::min(z,text.size()-file.at);std::memcpy(b,text.data()+file.at,n);file.at+=n;return n;},
        [](void* p,int64_t h,const void* b,size_t z)->int64_t {auto& f=get(p);f.data[f.handles.at(h).path].append(static_cast<const char*>(b),z);return z;},
        [](void*,int64_t){return 0;},[](void* p,int64_t h){get(p).handles.erase(h);return 0;},
        [](void* p,const char* n,port::settings::FileInfo& i){auto& f=get(p);if(!f.data.count(n))return -ENOENT;i={f.data[n].size(),true};return 0;},
        [](void* p,const char* a,const char* b){auto& f=get(p);if(!f.data.count(a))return -ENOENT;f.data[b]=f.data[a];f.data.erase(a);return 0;},
        [](void* p,const char* n){get(p).data.erase(n);return 0;}};}
};
