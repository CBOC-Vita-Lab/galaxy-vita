#include "port/player_settings.h"
#include <cassert>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <map>
using namespace port;
struct MemoryFiles {
    struct Handle { std::string path; size_t offset; bool write; };
    std::map<std::string,std::string> data;
    std::map<int64_t,Handle> handles;
    int64_t next=1;
    std::string failure;
    int renameCount=0, failRename=0;
    bool refuseExisting=false;
    static MemoryFiles& self(void* p) { return *static_cast<MemoryFiles*>(p); }
    settings::FileOps ops() { return {this,
        [](void* p,const char* n)->int64_t { auto& s=self(p); if(!s.data.count(n)) return -ENOENT; auto h=s.next++; s.handles[h]={n,0,false}; return h; },
        [](void* p,const char* n)->int64_t { auto& s=self(p); s.data[n]=""; auto h=s.next++; s.handles[h]={n,0,true}; return h; },
        [](void* p,int64_t h,void* b,size_t z)->int64_t { auto& s=self(p); auto& f=s.handles.at(h); auto& d=s.data.at(f.path); size_t n=std::min(z,d.size()-f.offset); if(s.failure=="read-short"&&n) --n; std::memcpy(b,d.data()+f.offset,n); f.offset+=n; return n; },
        [](void* p,int64_t h,const void* b,size_t z)->int64_t { auto& s=self(p); if(s.failure=="write") return -EIO; if(s.failure=="write-short") return 0; s.data[s.handles.at(h).path].append(static_cast<const char*>(b),z); return z; },
        [](void* p,int64_t)->int { return self(p).failure=="sync"?-EIO:0; },
        [](void* p,int64_t h)->int { auto& s=self(p); bool w=s.handles.at(h).write; s.handles.erase(h); return s.failure=="close"&&w?-EIO:0; },
        [](void* p,const char* n,settings::FileInfo& f)->int { auto& s=self(p); if(!s.data.count(n)) return -ENOENT; f={s.data[n].size(),true}; return 0; },
        [](void* p,const char* a,const char* b)->int { auto& s=self(p); if(++s.renameCount==s.failRename) return -EIO; if(!s.data.count(a)) return -ENOENT;if(s.refuseExisting&&s.data.count(b))return -EEXIST;s.data[b]=s.data[a]; s.data.erase(a); return 0; },
        [](void* p,const char* n)->int { auto& s=self(p);if((s.failure=="remove"&&std::string(n).find(".bak")!=std::string::npos)||(s.failure=="remove-primary"&&std::string(n)=="player-settings.cfg"))return -EIO;s.data.erase(n);return 0; }}; }
};
int main() {
    auto& api=controls::GetControlApi(); settings::Document d;
    auto initial=settings::DefaultConfig(api);initial.autoQuality=settings::AutoQuality::Enabled;initial.profile=controls::ProfileId::PlayStation;initial.language="French";initial.reducedMotion=true;initial.fpsLimit=30;
    auto complete=[&](const settings::Config& config){std::string text;assert(settings::SerializeDocument({settings::DefaultConfig(api),"schema=1\n",true},config,api,text).status==settings::StoreStatus::Ok);return text;};
    std::string source="# retained\r\nfuture = untouched\r\n";for(char ch:complete(initial)){if(ch=='\n')source+='\r';source+=ch;}
    assert(settings::ParseDocument(source,api,d).status==settings::StoreStatus::Ok);
    auto c=d.config; std::string out;
    assert(settings::SerializeDocument(d,c,api,out).status==settings::StoreStatus::Ok);
    assert(out.find("# retained\r\n")!=std::string::npos && out.find("future = untouched\r\n")!=std::string::npos);
    settings::Document round;
    assert(settings::ParseDocument(out,api,round).status==settings::StoreStatus::Ok && settings::Equal(c,round.config));
    for (const auto& bad : {"schema=1\nfps_limit=0\nfps_limit=30\n", "schema=9999999999999999999999\n", "schema=1\nreduced_motion=yes\n", "schema=1\nbind.handheld.jump=cross,circle\n"})
        assert(settings::ParseDocument(bad,api,d).status==settings::StoreStatus::Malformed);
    assert(settings::ParseDocument(std::string("schema=1\n\0",10),api,d).status==settings::StoreStatus::Malformed);
    assert(settings::ParseDocument("schema=1\nfps_limit",api,d).status==settings::StoreStatus::Malformed);
    assert(settings::ParseDocument("schema=1\nbind.handheld.jump",api,d).status==settings::StoreStatus::Malformed);
    assert(settings::ParseDocument(std::string(settings::kDocumentLimit+1,'x'),api,d).status==settings::StoreStatus::TooLarge);
    assert(settings::ParseDocument("schema=2\n",api,d).status==settings::StoreStatus::UnsupportedVersion && !d.editable);
    // The Display page's choices round-trip, are optional keys (a 1.1 or 1.2 file without them loads with the defaults and keeps its unknown lines),
    // reject bad values, and the file written fresh ends with a required key so a cut-short write is still seen as incomplete.
    {
        auto picked=settings::DefaultConfig(api);picked.aboveNative=false;picked.prioritizeMario=false;picked.antiAliasing=settings::AntiAliasing::Off;picked.sharpness=settings::Sharpness::Strong;
        picked.hdTextures=false;picked.effects=settings::EffectsQuality::Full;picked.depthOfField=false;picked.coreHelp=false;picked.tiltPercent=150;picked.tiltStick=true;
        std::string text;assert(settings::SerializeDocument({settings::DefaultConfig(api),"schema=1\n",true},picked,api,text).status==settings::StoreStatus::Ok);
        for(const char* key:{"above_native=0","mario_first=0","anti_aliasing=off","picture_sharpness=strong","hd_textures=0","effects_quality=full","depth_of_field=0","core_plan=off","motion_gain=150","motion_source=stick"})assert(text.find(key)!=std::string::npos);
        settings::Document back;assert(settings::ParseDocument(text,api,back).status==settings::StoreStatus::Ok&&settings::Equal(back.config,picked)&&settings::PresetOf(back.config)==settings::Preset::Custom);
        // A file from before these keys: strip them, the defaults come back and unknown lines survive a save.
        std::string legacy="# kept\nfuture=1\n";
        {size_t at=0;while(at<text.size()){auto end=text.find('\n',at)+1;const auto line=text.substr(at,end-at);
            bool drop=false;for(const char* key:{"above_native=","mario_first=","anti_aliasing=","picture_sharpness=","hd_textures=","effects_quality=","depth_of_field=","core_plan=","motion_gain=","motion_source="})drop|=line.rfind(key,0)==0;
            if(!drop)legacy+=line;at=end;}}
        settings::Document old;assert(settings::ParseDocument(legacy,api,old).status==settings::StoreStatus::Ok);
        assert(old.config.aboveNative&&old.config.prioritizeMario&&old.config.hdTextures&&old.config.depthOfField&&old.config.coreHelp&&old.config.tiltPercent==100&&!old.config.tiltStick&&old.config.antiAliasing==settings::AntiAliasing::Auto&&old.config.sharpness==settings::Sharpness::Auto&&old.config.effects==settings::EffectsQuality::Adaptive);
        auto newer=old.config;newer.hdTextures=false;std::string saved;assert(settings::SerializeDocument(old,newer,api,saved).status==settings::StoreStatus::Ok);
        assert(saved.find("# kept\nfuture=1\n")==0&&saved.find("hd_textures=0")!=std::string::npos);
        // Bad values.
        for(const char* bad:{"above_native=2","mario_first=yes","anti_aliasing=on","picture_sharpness=high","hd_textures=","effects_quality=half","depth_of_field=x","core_plan=on","motion_gain=110","motion_gain=25","motion_gain=225","motion_gain=","motion_gain=1e2","motion_gain=-100","motion_source=tilt","motion_source=","above_native=1\nabove_native=1"}) {
            std::string broken=text;const std::string b=bad;const auto k=b.substr(0,b.find('='));
            const auto at=broken.find("\n"+k+"=")+1;const auto end=broken.find('\n',at)+1;
            broken.replace(at,end-at,b+"\n");   // the bad line in place of the good one (the duplicate case brings its own second line)
            assert(settings::ParseDocument(broken,api,d).status==settings::StoreStatus::Malformed);
        }
        assert(text.back()=='\n'&&text.substr(text.rfind('\n',text.size()-2)+1).rfind("reduced_motion=",0)==0);
        for(size_t cut=0;cut<text.size();++cut)assert(settings::ParseDocument(text.substr(0,cut),api,d).status!=settings::StoreStatus::Ok);
    }
    const std::string old=complete(settings::DefaultConfig(api));
    for(const auto& failure : {"write", "write-short", "sync", "close", "read-short", ""}) for(int rename=0;rename<=2;++rename) {
        MemoryFiles fs; fs.data["player-settings.cfg"]=old; auto ops=fs.ops();
        assert(settings::LoadDocument("player-settings.cfg",api,ops,d).status==settings::StoreStatus::Ok);
        c=d.config; c.fpsLimit=30; fs.failure=failure; fs.failRename=rename;
        settings::Document saved; auto result=settings::SaveDocument("player-settings.cfg",d,c,api,ops,saved);
        if(failure[0]||rename) {
            assert(result.status!=settings::StoreStatus::Ok);
            fs.failure=""; fs.failRename=0;
            auto recovery=settings::LoadDocument("player-settings.cfg",api,ops,round);
            assert(recovery.status==settings::StoreStatus::Ok || recovery.status==settings::StoreStatus::Recovered);
            assert(round.config.fpsLimit==0);
        } else assert(result.status==settings::StoreStatus::Ok && fs.data["player-settings.cfg.bak"]==old);
        for(const auto& f:fs.data) assert(f.first.find(".ini")==std::string::npos && f.first.find("env.txt")==std::string::npos);
    }
    MemoryFiles fs; auto ops=fs.ops(); fs.data["player-settings.cfg"]="bad"; fs.data["player-settings.cfg.bak"]=old;
    assert(settings::LoadDocument("player-settings.cfg",api,ops,d).status==settings::StoreStatus::Recovered);
    c=d.config;c.fpsLimit=30;
    assert(settings::SaveDocument("player-settings.cfg",d,c,api,ops,round).status==settings::StoreStatus::Ok);
    assert(fs.data["player-settings.cfg.bak"]==old);
    fs.data["player-settings.cfg"]="schema=2\nfuture=keep\n";
    assert(settings::LoadDocument("player-settings.cfg",api,ops,d).status==settings::StoreStatus::UnsupportedVersion);
    assert(settings::SaveDocument("player-settings.cfg",d,c,api,ops,round).status==settings::StoreStatus::UnsupportedVersion);
    assert(fs.data["player-settings.cfg"]=="schema=2\nfuture=keep\n");
    // Vita-style rename refuses an existing target; a verified replacement can
    // remove a stale backup, but a removal error must not touch the primary.
    fs.data["player-settings.cfg"]=old;fs.data["player-settings.cfg.bak"]=complete(initial);fs.failure="remove";
    assert(settings::LoadDocument("player-settings.cfg",api,ops,d).status==settings::StoreStatus::Ok);
    assert(settings::SaveDocument("player-settings.cfg",d,c,api,ops,round).status==settings::StoreStatus::IoError);
    assert(fs.data["player-settings.cfg"]==old);
    fs.failure="";fs.refuseExisting=true;
    assert(settings::SaveDocument("player-settings.cfg",d,c,api,ops,round).status==settings::StoreStatus::Ok);
    fs.data["player-settings.cfg"]="broken";
    assert(settings::LoadDocument("player-settings.cfg",api,ops,d).status==settings::StoreStatus::Recovered);
    fs.failure="remove-primary";
    assert(settings::SaveDocument("player-settings.cfg",d,c,api,ops,round).status==settings::StoreStatus::IoError);
    assert(fs.data["player-settings.cfg.bak"]==old);
    auto nondefault=initial;const auto snapshot=complete(nondefault);
    for(size_t cut=0;cut<snapshot.size();++cut) {
        MemoryFiles interrupted;auto interruptedOps=interrupted.ops();interrupted.refuseExisting=true;
        interrupted.data["player-settings.cfg"]=snapshot.substr(0,cut);interrupted.data["player-settings.cfg.bak"]=snapshot;
        assert(settings::LoadDocument("player-settings.cfg",api,interruptedOps,d).status==settings::StoreStatus::Recovered);
        assert(settings::Equal(d.config,nondefault));auto next=d.config;next.reducedMotion=!next.reducedMotion;
        assert(settings::SaveDocument("player-settings.cfg",d,next,api,interruptedOps,round).status==settings::StoreStatus::Ok);
        assert(interrupted.data["player-settings.cfg.bak"]==snapshot&&round.config.fpsLimit==30&&round.config.language=="French");
    }
    fs.failure="";
    assert(settings::SaveDocument("player-settings.cfg",d,c,api,ops,round).status==settings::StoreStatus::Ok);
    assert(fs.data["player-settings.cfg.bak"]==old);
    std::cout<<"store: strict schema, preservation, bounded I/O, recovery and fault matrix passed\n";
}
