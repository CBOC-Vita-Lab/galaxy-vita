// Real Vita host; only the external SceIo boundary is an in-memory filesystem.
#include "../platform/vgx/vita/vgx_host_galaxy_vita.cpp"
#include "../platform/vgx/src/vgx_coldtrace.h"
#include <map>
#include <set>

namespace {
struct Handle { std::string path; size_t pos = 0; bool write = false; std::vector<std::string> children; };
std::map<std::string, std::vector<uint8_t>> files;
std::set<std::string> dirs;
std::map<int, Handle> handles;
int nextHandle = 10;
std::string readError, writeError, closeError, dirError, openError, statError;
unsigned writeFailures = 1, closeFailures = 1;
bool closeOnWriteOnly = false;
unsigned failedReadCloses = 0, failedWriteCloses = 0, personalOutputOpens = 0;
size_t readChunk = ~size_t(0), writeChunk = ~size_t(0);
unsigned directoryOpens = 0, inputOpens = 0, outputOpens = 0;
unsigned packageReads = 0;
unsigned shaderSources = 0;
constexpr const char* cache = "ux0:data/smg/shadercache/c0477cd7";
const std::string seed = std::string(kSeedDir) + "/c0477cd7";
const std::string packageList = std::string(kSeedDir) + "/vgx_shaderlist.bin";
const std::string marker = std::string(cache) + "/seeded";
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void Directory(const std::string& name) {
    dirs.insert(name);
    const auto slash = name.rfind('/');
    if (slash != std::string::npos) Directory(name.substr(0, slash));
}
void File(const std::string& name, std::vector<uint8_t> data) {
    files[name] = std::move(data); Directory(name.substr(0,name.rfind('/')));
}
std::vector<uint8_t> Text(const std::string& value) { return {value.begin(),value.end()}; }
void Fixture() {
    File(packageList, std::vector<uint8_t>(100, 0x31));
    File(seed + "/v2/v/00/111.gxp", {1,2,3,4});
    File(std::string(cache) + "/v2/f/02/personal.gxp", {9,8,7,6});
    File(kShaderListFile, Text("personal learned shader list"));
}
std::vector<std::string> Children(const std::string& parent) {
    std::set<std::string> names;
    auto add = [&](const std::string& path) {
        const std::string prefix = parent + '/';
        if (path.rfind(prefix,0) != 0) return;
        const std::string child = path.substr(prefix.size());
        if (!child.empty() && child.find('/') == std::string::npos) names.insert(child);
    };
    for (const auto& p : files) add(p.first);
    for (const auto& d : dirs) add(d);
    return {names.begin(), names.end()};
}
}

extern "C" void port_log(const char*, ...) {}
extern "C" { char vgl_shader_cache_path[256] = ""; }
extern "C" GLuint glCreateShader(GLenum) { static GLuint shader = 16; return ++shader; }
extern "C" void glShaderSource(GLuint, GLsizei count, const GLchar* const* text, const GLint* length) {
    std::string source;
    for (GLsizei i=0;i<count;++i) source.append(text[i],length && length[i]>=0 ? size_t(length[i]) : std::strlen(text[i]));
    Require(source=="abc","wrapper forwards source bytes and explicit/implicit lengths without including the NUL");
    ++shaderSources;
}
extern "C" SceUInt64 sceKernelGetProcessTimeWide() { return 1000000; }
extern "C" SceUID sceIoOpen(const char* name, int flags, SceMode) {
    const std::string path = name;
    if (path == openError) return static_cast<int>(0x8001000d);
    const bool write = (flags & SCE_O_WRONLY) != 0;
    if (write) {
        ++outputOpens;
        if(path==kShaderListFile)++personalOutputOpens;
        if (!files.count(path) && !(flags & SCE_O_CREAT)) return static_cast<int>(0x80010002);
        if (!dirs.count(path.substr(0,path.rfind('/')))) return static_cast<int>(0x80010002);
        if (flags & SCE_O_TRUNC) files[path].clear();
    } else {
        ++inputOpens;
        if (!files.count(path)) return static_cast<int>(0x80010002);
    }
    const int id = nextHandle++; handles[id] = {path,0,write,{}}; return id;
}
extern "C" int sceIoClose(SceUID id) {
    Require(handles.count(id), "only live file handles close");
    const bool fail = handles.at(id).path == closeError && closeFailures && (!closeOnWriteOnly || handles.at(id).write);
    if (fail) { --closeFailures; if(handles.at(id).write)++failedWriteCloses;else ++failedReadCloses; }
    handles.erase(id);
    return fail ? static_cast<int>(0x80010005) : 0;
}
extern "C" SceSSize sceIoRead(SceUID id, void* out, SceSize n) {
    Require(handles.count(id) && !handles.at(id).write, "read uses a live input handle");
    auto& h = handles.at(id);
    if(h.path==packageList)++packageReads;
    if (h.path == readError && h.pos) return static_cast<int>(0x80010005);
    const auto& data = files.at(h.path);
    const size_t count = std::min({size_t(n),data.size() - h.pos,readChunk});
    if (count) std::memcpy(out,data.data()+h.pos,count);
    h.pos += count; return static_cast<SceSSize>(count);
}
extern "C" SceSSize sceIoWrite(SceUID id, const void* in, SceSize n) {
    Require(handles.count(id) && handles.at(id).write, "write uses a live output handle");
    auto& h = handles.at(id); auto& data = files[h.path];
    const bool fail = h.path == writeError && writeFailures;
    const size_t count = fail ? std::min(size_t(n),writeChunk) : size_t(n);
    if (fail) --writeFailures;
    if (data.size() < h.pos + count) data.resize(h.pos + count);
    if (count) std::memcpy(data.data()+h.pos,in,count);
    h.pos += count; return static_cast<SceSSize>(count);
}
extern "C" int sceIoGetstat(const char* path, SceIoStat* out) {
    std::memset(out,0,sizeof *out);
    if (path == statError) return static_cast<int>(0x80010005);
    if (dirs.count(path)) { out->st_mode = SCE_S_IFDIR; return 0; }
    const auto it=files.find(path); if(it==files.end())return static_cast<int>(0x80010002);
    out->st_mode = SCE_S_IFREG; out->st_size = it->second.size(); return 0;
}
extern "C" int sceIoMkdir(const char* path, SceMode) {
    if(dirs.count(path))return static_cast<int>(0x80010011);
    Directory(path); return 0;
}
extern "C" int sceIoRemove(const char* path) { return files.erase(path) ? 0 : static_cast<int>(0x80010002); }
extern "C" SceUID sceIoDopen(const char* path) {
    ++directoryOpens;
    if(!dirs.count(path))return static_cast<int>(0x80010002);
    const int id=nextHandle++; handles[id]={path,0,false,Children(path)}; return id;
}
extern "C" int sceIoDread(SceUID id, SceIoDirent* out) {
    auto& h=handles.at(id);
    if(h.path==dirError && h.pos)return static_cast<int>(0x80010005);
    if(h.pos==h.children.size())return 0;
    const auto& name=h.children[h.pos++]; Require(name.size()<sizeof out->d_name,"fixture name fits SceIoDirent");
    std::strcpy(out->d_name,name.c_str()); sceIoGetstat((h.path+'/'+name).c_str(),&out->d_stat); return 1;
}
extern "C" int sceIoDclose(SceUID id) { return sceIoClose(id); }

int main(int argc, char** argv) {
    Require(argc==2,"supply seed-cache scenario"); const std::string mode=argv[1]; Fixture();
    const auto personal=files.at(kShaderListFile), personalGxp=files.at(std::string(cache)+"/v2/f/02/personal.gxp");
    if(mode=="bucket-early" || mode=="bucket-lengths") {
#if VGX_COLDTRACE
        auto& recorder=vgx::coldtrace::g_recorder;
        Require(recorder.Arm([]() noexcept -> uint64_t{return 1000000;},[]() noexcept -> uint32_t{return 1;},1),"arm source-hash annotation capture");
        {
        vgx::coldtrace::Scope trace(recorder,vgx::coldtrace::Key{1,1,0,0,0,0});
#endif
        const GLuint vertex=vgxvita_glCreateShader(GL_VERTEX_SHADER);
        const char* source="abc";
        if(mode=="bucket-early")vgxvita_glShaderSource(vertex,1,&source,nullptr);
        std::snprintf(vgl_shader_cache_path,sizeof vgl_shader_cache_path,"%s/v2",cache);
        if(mode=="bucket-early") {
            vgxvita_glShaderSource(vertex,1,&source,nullptr);
            Require(shaderSources==2,"early shader source still reaches the GL boundary");
        } else {
            const char* parts[]={"a","bcTAIL"}; const GLint sizes[]={-1,2};
            vgxvita_glShaderSource(vertex,2,parts,sizes);
            const GLuint fragment=vgxvita_glCreateShader(GL_FRAGMENT_SHADER);
            const GLint size=3; source="abcTAIL"; vgxvita_glShaderSource(fragment,1,&source,&size);
            Require(!dirs.count(std::string(cache)+"/v2/f/78"),"fragment source does not create a directory before a cache miss is known");
        }
        // XXH3_64bits("abc",3) = 78af5f94892f3950, independently fixed fixture.
        Require(!dirs.count(std::string(cache)+"/v2/v/78"),"shader source does no directory I/O before the native cache lookup");
#if VGX_COLDTRACE
        }
        recorder.RequestFreeze();const auto view=recorder.Frozen();
        Require(view.data && view.data->worstCount==1,"source hash annotations belong to the current shader work item");
        const auto& item=view.data->worst[0];
        Require(item.shaderCount==(mode=="bucket-lengths"?2u:1u) && item.shaders[0].sourceHash==0x78af5f94892f3950ull && item.shaders[0].kind==1,"annotations reuse the exact GLSL GXP hash without shader text or extra hashing");
        if(mode=="bucket-lengths")Require(item.shaders[1].sourceHash==item.shaders[0].sourceHash && item.shaders[1].kind==2,"vertex and fragment hash annotations stay distinct");
#endif
    } else if(mode=="stamp-io-count") {
        files[packageList].resize(313336,0x31); char stamp[80];
        Require(PackageStamp(stamp,sizeof stamp) && packageReads==6,"packaged-size fingerprint uses five bounded bulk reads plus EOF, not dozens of small reads");
    } else if(mode=="stamp-known" || mode=="stamp-chunks") {
        files[packageList]=Text("abc"); char stamp[80];
        Require(PackageStamp(stamp,sizeof stamp) && std::strcmp(stamp,"list2 3 44bc2cf5ad770999")==0,"XXH64 stamp algorithm and wire format stay pinned to a known answer");
        if(mode=="stamp-chunks") {
            files[packageList].resize(10000);
            for(size_t i=0;i<files[packageList].size();++i)files[packageList][i]=static_cast<uint8_t>(i*7u);
            readChunk=4095;
            char expected[80]; std::snprintf(expected,sizeof expected,"list2 10000 %016llx",static_cast<unsigned long long>(XXH64(files[packageList].data(),files[packageList].size(),0)));
            Require(PackageStamp(stamp,sizeof stamp) && std::strcmp(stamp,expected)==0,"streamed fingerprint matches one-shot hash across short-read boundaries");
        }
    } else if(mode=="equal-size") {
        char first[80],second[80]; PackageStamp(first,sizeof first);
        files[packageList][50]^=1; PackageStamp(second,sizeof second);
        Require(std::strcmp(first,second)!=0,"equal-size packages with different shader-list bytes have different stamps");
    } else if(mode=="copy-short-read" || mode=="copy-read-error" || mode=="copy-close-error" || mode=="copy-short-write") {
        const std::string from=seed+"/v2/v/00/111.gxp", to=std::string(cache)+"/copy.gxp";
        files[from]=std::vector<uint8_t>(100,0x51);
        if(mode=="copy-short-read" || mode=="copy-read-error")readChunk=3;
        if(mode=="copy-read-error")readError=from;
        if(mode=="copy-close-error")closeError=to;
        if(mode=="copy-short-write"){writeError=to;writeChunk=2;}
        SeedCopyStats st; const bool ok=CopyFile(from.c_str(),to.c_str(),false,st);
        if(mode=="copy-short-read")Require(ok && files.at(to)==files.at(from),"positive short reads continue until actual EOF");
        else Require(!ok && !files.count(to),"failed read/write/close never leaves a half GXP accepted as complete");
    } else if(mode=="legacy" || mode=="marker-nul" || mode=="marker-short-write" || mode=="zero-gxp" || mode=="existing-gxp") {
        if(mode=="legacy") {
            File(marker,Text("list 100")); files[packageList][50]^=1;
            File(seed+"/v2/v/00/222.gxp",{5,6});
            SeedShaderCache(cache); SeedShaderList();
            Require(files.count(std::string(cache)+"/v2/v/00/222.gxp"),"legacy size match cannot conceal a changed package's new GXP");
            Require(files.at(kShaderListFile)==personal,"marker-format migration preserves the personal learned list");
        } else if(mode=="marker-nul") {
            SeedShaderCache(cache); files[marker].insert(files[marker].end(),{0,'x'});
            File(seed+"/v2/v/00/222.gxp",{5,6}); SeedShaderCache(cache);
            Require(files.count(std::string(cache)+"/v2/v/00/222.gxp"),"embedded NUL cannot make a malformed marker match a valid fingerprint");
        } else if(mode=="marker-short-write") {
            writeError=marker; writeChunk=3; SeedShaderCache(cache);
            Require(!files.count(marker) && !g_seedIsNew,"short marker write cannot publish a complete seed or authorize list adoption");
        } else {
            const std::string to=std::string(cache)+"/v2/v/00/111.gxp";
            File(to, mode=="zero-gxp" ? std::vector<uint8_t>{} : std::vector<uint8_t>{0x7f});
            SeedShaderCache(cache);
            Require(files.at(to)==(mode=="zero-gxp" ? std::vector<uint8_t>{1,2,3,4} : std::vector<uint8_t>{0x7f}),
                    "empty interrupted GXPs are repaired while nonempty existing GXPs are retained");
        }
    } else if(mode=="list-stat-error") {
        SeedShaderCache(cache); const auto good=files.at(marker);
        const unsigned beforeWrites=outputOpens; statError=kShaderListFile;
        SeedShaderList();
        Require(files.count(marker) && files.at(marker)==good,"uncertain personal-list stat does not discard an otherwise valid seed marker");
        Require(files.at(kShaderListFile)==personal && outputOpens==beforeWrites,"uncertain existing list is never opened for replacement");
    } else if(mode=="list-override") {
        SeedShaderCache(cache); files[packageList][50]^=1;
        setenv("GXV_SHADERLIST","custom-list",1); SeedShaderCache(cache);
        Require(!g_seedIsNew && files.at(kShaderListFile)==personal,"explicit list override does not schedule adoption of the default learned list");
        directoryOpens=0; SeedShaderCache(cache);
        Require(directoryOpens==0,"explicit list override still checkpoints a completed compiled-cache copy");
    } else if(mode=="adopt-restore-fail" || mode=="adopt-marker-fail") {
        SeedShaderCache(cache); const auto oldMarker=files.at(marker);
        files[packageList][50]^=1; SeedShaderCache(cache);
        Require(files.at(marker)==oldMarker,"changed-package copy does not publish a new marker before adoption");
        writeError=mode=="adopt-restore-fail"?kShaderListFile:marker; writeChunk=3;
        if(mode=="adopt-restore-fail")writeFailures=2;
        SeedShaderList();
        Require(!g_seedIsNew,"failed commit or restore clears transient adoption state");
        if(mode=="adopt-restore-fail")Require(files.at(marker)==oldMarker,"failed rollback retains old marker to select repair on restart");
        else Require(files.at(kShaderListFile)==files.at(packageList) && !files.count(marker),"marker failure happens after successful list adoption");
        const unsigned beforeWrites=personalOutputOpens;
        writeError.clear(); directoryOpens=0; SeedShaderCache(cache); SeedShaderList();
        Require(files.at(kShaderListFile)==files.at(packageList),"a restart recovers the regenerable list after rollback or marker failure");
        if(mode=="adopt-marker-fail")Require(directoryOpens>0 && personalOutputOpens==beforeWrites,"marker repair validates cache without re-adopting an already updated list");
        char stamp[80]; Require(PackageStamp(stamp,sizeof stamp) && files.at(marker)==Text(stamp),"recovered startup commits the current fingerprint");
    } else if(mode=="adopt-retry" || mode=="adopt-close-retry" || mode=="adopt-crash-before-list" || mode=="adopt-large") {
        if(mode=="adopt-large")File(kShaderListFile,std::vector<uint8_t>(kSeedMaxListBytes+1u,0x8e));
        const auto before=files.at(kShaderListFile);
        SeedShaderCache(cache); const auto oldMarker=files.at(marker); files[packageList][50]^=1; SeedShaderCache(cache);
        Require(g_seedIsNew,"changed package selects list adoption");
        Require(files.at(marker)==oldMarker,"pending adoption keeps the prior fingerprint intact");
        if(mode=="adopt-retry" || mode=="adopt-close-retry") {
            if(mode=="adopt-close-retry"){closeError=kShaderListFile;closeOnWriteOnly=true;}
            else {writeError=kShaderListFile;writeChunk=3;}
            SeedShaderList(); writeError.clear(); closeError.clear();
            if(mode=="adopt-close-retry")Require(failedWriteCloses==1 && failedReadCloses==0,"replacement close fails after a successful pre-read and before rollback");
            Require(files.at(kShaderListFile)==before,"one-shot failed adoption restores the personal list before retry");
        } else if(mode=="adopt-large") {
            SeedShaderList();
            Require(files.at(kShaderListFile)==before,"oversized personal list is preserved without truncation or unbounded rollback memory");
        }
        g_seedIsNew=false; directoryOpens=0; SeedShaderCache(cache); SeedShaderList();
        if(mode=="adopt-large")Require(directoryOpens==0 && files.at(kShaderListFile)==before,"oversized personal list does not cause a seed traversal on every boot");
        else Require(files.at(kShaderListFile)==files.at(packageList),"failed or interrupted adoption is retried after the next startup");
        char stamp[80]; Require(PackageStamp(stamp,sizeof stamp) && files.at(marker)==Text(stamp),"completed adoption publishes the current fingerprint");
    } else if(mode=="adopt-read-error" || mode=="adopt-write-error" || mode=="adopt-close-error" || mode=="adopt-preread-close-error" ||
              mode=="adopt-stat-error" || mode=="adopt-missing") {
        // A valid prior marker from a different package makes this a real content update, not a format migration.
        SeedShaderCache(cache); const auto oldMarker=files.at(marker); files[packageList][50]^=1; SeedShaderCache(cache);
        Require(g_seedIsNew,"completed changed-package copy selects checked list adoption");
        if(mode=="adopt-read-error"){readError=packageList;readChunk=3;}
        if(mode=="adopt-write-error"){writeError=kShaderListFile;writeChunk=3;}
        if(mode=="adopt-close-error" || mode=="adopt-preread-close-error")closeError=kShaderListFile;
        if(mode=="adopt-close-error")closeOnWriteOnly=true;
        if(mode=="adopt-stat-error")statError=packageList;
        if(mode=="adopt-missing")files.erase(packageList);
        SeedShaderList();
        if(mode=="adopt-close-error")Require(failedWriteCloses==1 && failedReadCloses==0,"adoption close fault targets the replacement output rather than the pre-read");
        if(mode=="adopt-preread-close-error")Require(failedReadCloses==1 && failedWriteCloses==0,"pre-read close failure never reaches a target write");
        Require(files.at(kShaderListFile)==personal,"failed list adoption keeps or restores the original learned bytes without a backup file");
        Require(files.count(marker) && files.at(marker)==oldMarker,"failed list adoption keeps the old fingerprint so the replacement can retry");
        Require(!g_seedIsNew,"failed list adoption clears the transient replacement state for retry");
    } else if(mode=="same" || mode=="changed" || mode=="dir-error" || mode=="cap" || mode=="cap-exact" || mode=="partial-list") {
        if(mode=="cap")for(unsigned i=0;i<4097;++i)File(seed+"/extra-"+std::to_string(i)+".gxp",{1});
        if(mode=="cap-exact")for(unsigned i=0;i<4095;++i)File(seed+"/extra-"+std::to_string(i)+".gxp",{1});
        if(mode=="dir-error" || mode=="partial-list")dirError=seed+"/v2/v";
        SeedShaderCache(cache);
        if(mode=="dir-error" || mode=="cap" || mode=="partial-list") {
            Require(!files.count(marker),"incomplete seed traversal must not write a completed seed marker");
            Require(!g_seedIsNew,"partial seed copy cannot authorize replacement of personal learned list");
            SeedShaderList(); Require(files.at(kShaderListFile)==personal,"partial copy preserves the personal warm-up list");
        } else {
            Require(files.count(marker),"complete seed copy writes a marker");
            directoryOpens=0; const unsigned beforeWrites=outputOpens;
            if(mode=="changed") {
                files[packageList][50]^=1; File(seed+"/v2/v/00/222.gxp",{5,6});
                SeedShaderCache(cache);
                Require(files.count(std::string(cache)+"/v2/v/00/222.gxp"),"same-size changed package copies newly packaged compiled shaders");
            } else {
                SeedShaderCache(cache);
                Require(directoryOpens==0 && outputOpens==beforeWrites,"unchanged package avoids directory rescans and writes");
            }
        }
    } else if(mode=="stamp-read-error" || mode=="stamp-close-error") {
        SeedShaderCache(cache); const auto good=files.at(marker); g_seedIsNew=false;
        if(mode=="stamp-read-error"){readError=packageList;readChunk=3;}else closeError=packageList;
        files[packageList][50]^=1; SeedShaderCache(cache);
        Require(!g_seedIsNew && files.at(marker)==good,"uncertain package fingerprint cannot publish new-copy status or a new marker");
    } else Require(false,"known seed-cache scenario");
    Require(files.at(std::string(cache)+"/v2/f/02/personal.gxp")==personalGxp,"existing personal compiled shaders are retained");
    Require(handles.empty(),"all opened file/directory handles are closed");
    std::printf("PASS: seed-cache %s (synthetic SceIo filesystem, probe=%d, not Vita timings)\n",mode.c_str(),VGX_COLDTRACE);
}
