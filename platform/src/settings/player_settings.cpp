#include "port/player_settings.h"
#include <algorithm>
#include <charconv>
#include <map>
#include <set>

namespace port::settings {
namespace {
struct Line { std::string raw, key, value, ending; };
std::string Trim(std::string s) {
    const auto first=s.find_first_not_of(" \t\r");
    return first==std::string::npos ? "" : s.substr(first,s.find_last_not_of(" \t\r")-first+1);
}
std::vector<Line> Lines(const std::string& source) {
    std::vector<Line> lines;
    for(size_t at=0;at<source.size();) {
        auto end=source.find('\n',at); bool newline=end!=std::string::npos;
        if(!newline) end=source.size();
        Line l; l.raw=source.substr(at,end-at+(newline?1:0));
        l.ending=newline?(end>at&&source[end-1]=='\r'?"\r\n":"\n"):"";
        auto body=Trim(source.substr(at,end-at));
        if(!body.empty()&&body[0]!='#'&&body[0]!=';') {
            auto eq=body.find('=');
            if(eq!=std::string::npos) { l.key=Trim(body.substr(0,eq)); l.value=Trim(body.substr(eq+1)); }
        }
        lines.push_back(std::move(l));at=end+(newline?1:0);
    }
    return lines;
}
bool KnownAction(const controls::ControlApi& api,const std::string& s) {
    for(size_t i=0;i<api.actionCount;++i) if(s==api.actions[i].id) return true;
    return false;
}
bool Managed(const std::string& key,const controls::ControlApi& api) {
    if(key=="schema"||key=="fps_limit"||key=="auto_quality"||key=="profile"||key=="language"||key=="reduced_motion") return true;
    for(const auto* p:{"bind.handheld.","bind.playstation."})
        if(key.compare(0,std::char_traits<char>::length(p),p)==0&&KnownAction(api,key.substr(std::char_traits<char>::length(p)))) return true;
    return false;
}
StoreResult Error(StoreStatus s,const std::string& message) { return {s,message}; }
bool ValidConfig(const Config& c,const controls::ControlApi& api,std::string& error) {
    if(c.version!=1 || (c.fpsLimit!=0&&c.fpsLimit!=30&&c.fpsLimit!=kSteady30) || static_cast<unsigned>(c.autoQuality)>2 || static_cast<unsigned>(c.profile)>1) {error="Invalid settings value";return false;}
    if(c.language.size()>64 || std::any_of(c.language.begin(),c.language.end(),[](unsigned char ch){return ch<32||ch>126||ch=='=';})) {error="Invalid language name";return false;}
    for(unsigned i=0;i<2;++i) if(!api.validate(c.profiles[i],static_cast<controls::ProfileId>(i),controls::NominalCaps(static_cast<controls::ProfileId>(i)),error)) return false;
    return true;
}
std::map<std::string,std::string> Values(const Config& c) {
    std::map<std::string,std::string> v{{"schema","1"},{"fps_limit",c.fpsLimit==kSteady30?std::string("steady30"):std::to_string(c.fpsLimit)},
        {"auto_quality",c.autoQuality==AutoQuality::Default?"default":c.autoQuality==AutoQuality::Enabled?"on":"off"},
        {"profile",c.profile==controls::ProfileId::Handheld?"handheld":"playstation"},
        {"language",c.language},{"reduced_motion",c.reducedMotion?"1":"0"}};
    for(unsigned i=0;i<2;++i) for(const auto& b:c.profiles[i].bindings)
        v[std::string("bind.")+(i?"playstation.":"handheld.")+b.action]=b.buttons[0]+(b.buttons[1].empty()?"":","+b.buttons[1]);
    return v;
}
}
Config DefaultConfig(const controls::ControlApi& api) {
    Config c; for(unsigned i=0;i<2;++i)c.profiles[i]=api.defaults(static_cast<controls::ProfileId>(i));return c;
}
bool Equal(const Config& a,const Config& b) { return Values(a)==Values(b) && a.version==b.version; }
StoreResult ParseDocument(const std::string& source,const controls::ControlApi& api,Document& result) {
    Document d{DefaultConfig(api),source,true};
    if(source.size()>kDocumentLimit) return Error(StoreStatus::TooLarge,"Settings file exceeds 16384 bytes");
    if(source.find('\0')!=std::string::npos) return Error(StoreStatus::Malformed,"Settings file contains a NUL byte");
    const auto lines=Lines(source); std::set<std::string> seen;
    for(const auto& l:lines)if(l.key.empty()&&Managed(Trim(l.raw),api))return Error(StoreStatus::Malformed,"Incomplete setting");
    for(const auto& l:lines) if(Managed(l.key,api)) {
        if(!seen.insert(l.key).second) return Error(StoreStatus::Malformed,"Duplicate setting: "+l.key);
        if(l.key=="schema") {
            uint32_t v=0; auto parsed=std::from_chars(l.value.data(),l.value.data()+l.value.size(),v);
            if(parsed.ec!=std::errc{}||parsed.ptr!=l.value.data()+l.value.size()||v==0) return Error(StoreStatus::Malformed,"Invalid schema");
            if(v>1) { d.config.version=v;d.editable=false;result=std::move(d);return Error(StoreStatus::UnsupportedVersion,"Newer settings file is read-only"); }
        }
    }
    if(!seen.count("schema")) return Error(StoreStatus::Malformed,"Missing schema");
    // Schema 1 is always a complete snapshot, not a sparse preferences overlay.
    // This distinguishes interrupted writes (including newline boundaries) from
    // intentional defaults before a primary may displace a valid backup.
    for(const auto& value:Values(d.config))if(!seen.count(value.first))return Error(StoreStatus::Malformed,"Incomplete settings snapshot: "+value.first);
    if(source.empty()||source.back()!='\n')return Error(StoreStatus::Malformed,"Incomplete settings terminator");
    for(const auto& l:lines) {
        const auto& k=l.key; const auto& v=l.value;
        if(k=="fps_limit") { if(v!="0"&&v!="30"&&v!="steady30")return Error(StoreStatus::Malformed,"Invalid picture rate");d.config.fpsLimit=v=="30"?30:v=="steady30"?kSteady30:0; }
        else if(k=="auto_quality") { if(v!="default"&&v!="on"&&v!="off") return Error(StoreStatus::Malformed,"Invalid dynamic quality");d.config.autoQuality=v=="default"?AutoQuality::Default:v=="on"?AutoQuality::Enabled:AutoQuality::Disabled; }
        else if(k=="profile") { if(v!="handheld"&&v!="playstation")return Error(StoreStatus::Malformed,"Invalid control profile");d.config.profile=v=="handheld"?controls::ProfileId::Handheld:controls::ProfileId::PlayStation; }
        else if(k=="language") d.config.language=v;
        else if(k=="reduced_motion") { if(v!="0"&&v!="1")return Error(StoreStatus::Malformed,"Invalid reduced motion");d.config.reducedMotion=v=="1"; }
        else if(Managed(k,api)&&k.compare(0,5,"bind.")==0) {
            const bool ps=k.compare(5,12,"playstation.")==0;
            const auto action=k.substr(ps?17:14); auto comma=v.find(',');
            if(comma!=std::string::npos&&v.find(',',comma+1)!=std::string::npos)return Error(StoreStatus::Malformed,"Too many bindings");
            for(auto& b:d.config.profiles[ps?1:0].bindings) if(b.action==action) {
                b.buttons={comma==std::string::npos?v:v.substr(0,comma),comma==std::string::npos?"":v.substr(comma+1)};
                if(b.buttons[0].empty()||(comma!=std::string::npos&&b.buttons[1].empty())) return Error(StoreStatus::Malformed,"Empty binding");
            }
        }
    }
    std::string error;
    if(!ValidConfig(d.config,api,error))return Error(StoreStatus::Malformed,error);
    result=std::move(d);return {StoreStatus::Ok,""};
}
StoreResult SerializeDocument(const Document& original,const Config& c,const controls::ControlApi& api,std::string& out) {
    if(!original.editable||original.config.version>1)return Error(StoreStatus::UnsupportedVersion,"Newer settings file is read-only");
    std::string error;if(!ValidConfig(c,api,error))return Error(StoreStatus::Malformed,error);
    auto values=Values(c);std::string serialized;
    for(const auto& l:Lines(original.source)) {
        auto value=values.find(l.key);
        if(value==values.end())serialized+=l.raw;
        else {serialized+=l.key+"="+value->second+l.ending;values.erase(value);}
    }
    if(!values.empty()&&!serialized.empty()&&serialized.back()!='\n')serialized+='\n';
    for(const auto& v:values)serialized+=v.first+"="+v.second+"\n";
    if(serialized.size()>kDocumentLimit)return Error(StoreStatus::TooLarge,"Settings file exceeds 16384 bytes");
    Document check;auto parsed=ParseDocument(serialized,api,check);if(parsed.status!=StoreStatus::Ok)return parsed;
    out=std::move(serialized);return {StoreStatus::Ok,""};
}
}
