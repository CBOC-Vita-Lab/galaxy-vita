#include "port/player_settings.h"
#include <cerrno>

namespace port::settings {
namespace {
StoreResult Io(const char* op) { return {StoreStatus::IoError,std::string("Settings ")+op+" failed"}; }
StoreResult Read(const std::string& path,const controls::ControlApi& api,const FileOps& f,Document& d) {
    FileInfo info{};int stat=f.stat(f.context,path.c_str(),info);
    if(stat==-ENOENT)return {StoreStatus::Missing,"No saved player settings"};
    if(stat<0)return Io("stat");
    if(!info.regular)return Io("file type");
    if(info.size>kDocumentLimit)return {StoreStatus::TooLarge,"Settings file exceeds 16384 bytes"};
    int64_t h=f.openRead(f.context,path.c_str());if(h<0)return Io("open");
    std::string data(static_cast<size_t>(info.size),'\0');size_t at=0;bool complete=true;
    while(at<data.size()) {auto n=f.read(f.context,h,data.data()+at,data.size()-at);if(n<=0||static_cast<uint64_t>(n)>data.size()-at){complete=false;break;}at+=static_cast<size_t>(n);}
    char extra=0;if(complete&&f.read(f.context,h,&extra,1)!=0)complete=false;
    if(f.close(f.context,h)<0)return Io("read close");
    if(!complete)return Io("complete read");
    return ParseDocument(data,api,d);
}
}
StoreResult LoadDocument(const char* path,const controls::ControlApi& api,const FileOps& f,Document& out) {
    out={DefaultConfig(api),"",true};
    Document primary;auto r=Read(path,api,f,primary);
    if(r.status==StoreStatus::Ok||r.status==StoreStatus::UnsupportedVersion) {out=std::move(primary);return r;}
    Document backup;auto b=Read(std::string(path)+".bak",api,f,backup);
    if(b.status==StoreStatus::Ok) {out=std::move(backup);return {StoreStatus::Recovered,"Recovered last valid player settings"};}
    if(r.status==StoreStatus::Missing&&b.status==StoreStatus::UnsupportedVersion){out=std::move(backup);return b;}
    return r;
}
StoreResult SaveDocument(const char* path,const Document& original,const Config& config,const controls::ControlApi& api,const FileOps& f,Document& saved) {
    std::string text;auto r=SerializeDocument(original,config,api,text);if(r.status!=StoreStatus::Ok)return r;
    Document current;auto primary=Read(path,api,f,current);
    if(primary.status==StoreStatus::UnsupportedVersion)return primary;
    // Never replace an unreadable file: it may be a valid newer document.
    if(primary.status==StoreStatus::IoError)return primary;
    const std::string temporary=std::string(path)+".tmp", backup=std::string(path)+".bak";
    auto h=f.openWrite(f.context,temporary.c_str());if(h<0)return Io("temporary open");
    size_t at=0;bool written=true;
    while(at<text.size()) {auto n=f.write(f.context,h,text.data()+at,text.size()-at);if(n<=0||static_cast<uint64_t>(n)>text.size()-at){written=false;break;}at+=static_cast<size_t>(n);}
    int sync=written?f.sync(f.context,h):-1;int close=f.close(f.context,h);
    if(!written||sync<0||close<0) {f.remove(f.context,temporary.c_str());return Io(!written?"write":sync<0?"sync":"write close");}
    Document verify;auto checked=Read(temporary,api,f,verify);
    if(checked.status!=StoreStatus::Ok||!Equal(verify.config,config)||verify.source!=text) {f.remove(f.context,temporary.c_str());return Io("temporary verification");}
    if(primary.status==StoreStatus::Ok) {
        // Vita rename need not replace an existing destination. At this point
        // both the primary and the fully verified temporary are valid copies.
        int removed=f.remove(f.context,backup.c_str());
        if(removed<0&&removed!=-ENOENT) {f.remove(f.context,temporary.c_str());return Io("old backup removal");}
        if(f.rename(f.context,path,backup.c_str())<0) {f.remove(f.context,temporary.c_str());return Io("backup rename");}
    } else if(primary.status!=StoreStatus::Missing) {
        // A malformed primary must not replace the valid retained backup. The
        // verified temporary is ready before removing only that invalid file.
        int removed=f.remove(f.context,path);
        if(removed<0&&removed!=-ENOENT) {f.remove(f.context,temporary.c_str());return Io("invalid primary removal");}
    }
    if(f.rename(f.context,temporary.c_str(),path)<0) {f.remove(f.context,temporary.c_str());return Io("replacement rename");}
    saved=std::move(verify);return {StoreStatus::Ok,"Saved for next launch"};
}
}
