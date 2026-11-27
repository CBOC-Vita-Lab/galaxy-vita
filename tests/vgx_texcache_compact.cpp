#include "vgx_state.h"
#include "vgx_texdecode.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}

namespace {
unsigned patchCalls = 0;
unsigned predicateCalls = 0;
uint64_t expectedHash = 0, patchHash = 0;
uint32_t expectedWidth = 0, expectedHeight = 0;
bool predicateResult = false;
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
bool NeedsRgba(uint64_t hash, uint32_t w, uint32_t h) {
    ++predicateCalls;
    Require(hash==expectedHash && w==expectedWidth && h==expectedHeight,"predicate receives actual content hash and dimensions");
    return predicateResult;
}
void Patch(uint64_t hash, uint32_t w, uint32_t h, uint8_t* rgba) {
    ++patchCalls;
    patchHash=hash;
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        rgba[4*i] = 17; rgba[4*i+1] = 109; rgba[4*i+2] = 233; rgba[4*i+3] = 67;
    }
}
std::vector<uint8_t> Data(unsigned fmt, unsigned w, unsigned h) {
    std::vector<uint8_t> bytes(vgx::TexLevelSize(w,h,fmt));
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (fmt == 0) bytes[i] = 0x29;
        if (fmt == 1) bytes[i] = i % 2 ? 0xB7 : 0x25;
        if (fmt == 2) bytes[i] = i % 2 ? 0x5A : 0xC3;
        if (fmt == 3) {
            constexpr uint8_t pair[] = {0xE1,0x47,0x26,0xB8};
            bytes[i] = pair[i % 4];
        }
    }
    return bytes;
}
void Check(const vgx::TexUpload& up, unsigned fmt, bool patched) {
    const bool compact = up.format == vgx::TF_LA88;
    Require(up.levels.size() == 1 && !up.genMips && up.authored == 1,"base-only texture keeps one authored level");
    Require(up.levels[0].size() == size_t(up.width)*up.height*(compact?2:4),"compact payload uses exactly half the RGBA bytes");
    constexpr uint8_t luminance[][2] = {{34,153},{37,183},{51,170},{71,184}};
    constexpr uint8_t alpha[][2] = {{34,153},{37,183},{204,85},{225,38}};
    for (unsigned y = 0; y < up.height; ++y) for (unsigned x = 0; x < up.width; ++x) {
        const auto* px = up.levels[0].data() + (size_t(y)*up.width+x)*(compact?2:4);
        if (patched) Require(!compact && px[0]==17 && px[1]==109 && px[2]==233 && px[3]==67,
                             "colored patch pixels survive compact-format selection");
        else if (compact) Require(px[0]==luminance[fmt][x%2] && px[1]==alpha[fmt][x%2],
                                  "compact decode preserves independent luminance and alpha at tile edges");
        else Require(px[0]==luminance[fmt][x%2] && px[1]==px[0] && px[2]==px[0] && px[3]==alpha[fmt][x%2],
                     "RGBA reference preserves all hand-checked fixture pixels");
    }
}
}

int main(int argc, char** argv) {
    Require(argc==3,"usage: compact plain|legacy|accept|reject|mip expected-la88");
    Require(vgx::backend::GetCaps().la88==(std::string(argv[2])=="1"),"explicit cap control really enables or disables compact uploads");
    const std::string mode=argv[1];
    if (mode=="switch") {
        auto bytes=Data(1,8,8);
        vgx::ResetState(); vgx::g.bpTexStale=0; vgx::Pending().clear();
        auto& slot=vgx::g.tex[0]; slot.valid=true; slot.avail=bytes.size();
        slot.obj.data=bytes.data(); slot.obj.width=slot.obj.height=8; slot.obj.format=1;
        slot.obj.minFilter=vgx::FILT_LINEAR; slot.obj.magFilter=1;
        vgx::SetTexturePatchHook(Patch,NeedsRgba);
        uint32_t id=0;
        for (unsigned step=0;step<3;++step) {
            bytes[0]=step==1?38:37;
            expectedHash=XXH3_64bits(bytes.data(),bytes.size()); expectedWidth=expectedHeight=8;
            predicateResult=step!=1;
            vgx::TexBind binding; uint16_t w=0,h=0;
            Require(vgx::ResolveTexture(0,binding,w,h) && vgx::Pending().uploads.size()==step+1,
                    "changed content at the same address is re-uploaded exactly once");
            if (!step) id=binding.texId;
            Require(binding.texId==id,"content-dependent format switch preserves cache texture identity");
            const auto& up=vgx::Pending().uploads.back();
            const bool compact=step==1 && vgx::backend::GetCaps().la88;
            Require(up.format==(compact?vgx::TF_LA88:vgx::TF_RGBA8),"predicate flips RGBA to compact and back on one cache entry");
            Require(up.levels[0][0]==(compact?38:17),"format switch publishes the new decoded or patched pixels");
        }
        Require(patchCalls==(vgx::backend::GetCaps().la88?2u:3u),"only RGBA respecifications invoke the patch");
        std::puts("PASS: one real cache entry switches RGBA/LA88/RGBA on content changes");
        return 0;
    }
    if (mode=="follow") {
        // settingsnative: the button pictures follow a swapped mapping. The patch makes a picture that depends on a setting; when the setting
        // changes and the renderer is told (TexturePatchChanged), a texture the patch handles is decoded and patched again at its next use,
        // once, in place (same texture id); one the patch does not handle is not touched.
        auto bytes=Data(1,8,8), other=Data(2,8,8);
        vgx::ResetState(); vgx::g.bpTexStale=0; vgx::Pending().clear();
        for (unsigned i=0;i<2;++i) {
            auto& slot=vgx::g.tex[i]; auto& d=i?other:bytes;
            slot.valid=true; slot.avail=d.size();
            slot.obj.data=d.data(); slot.obj.width=slot.obj.height=8; slot.obj.format=i?2:1;
            slot.obj.minFilter=vgx::FILT_LINEAR; slot.obj.magFilter=1;
        }
        const uint64_t mine=XXH3_64bits(bytes.data(),bytes.size());
        static uint64_t sMine; sMine=mine;
        static uint8_t sShade; sShade=17;
        struct F { static bool Needs(uint64_t h,uint32_t,uint32_t){ return h==sMine; }
                   static void P(uint64_t h,uint32_t w,uint32_t hh,uint8_t* rgba){ if(h!=sMine) return; for(size_t i=0;i<size_t(w)*hh;++i){rgba[4*i]=sShade;rgba[4*i+1]=1;rgba[4*i+2]=2;rgba[4*i+3]=255;} } };
        vgx::SetTexturePatchHook(F::P,F::Needs);
        vgx::TexBind b0,b1; uint16_t w=0,h=0;
        Require(vgx::ResolveTexture(0,b0,w,h) && vgx::ResolveTexture(1,b1,w,h) && vgx::Pending().uploads.size()==2,"both textures uploaded once");
        Require(vgx::Pending().uploads[0].levels[0][0]==17,"the patched picture starts with the first setting");
        vgx::TexBind c; 
        Require(vgx::ResolveTexture(0,c,w,h) && vgx::ResolveTexture(1,c,w,h) && vgx::Pending().uploads.size()==2,"no change: a cache hit, nothing decoded again");
        sShade=99;
        Require(vgx::ResolveTexture(0,c,w,h) && vgx::Pending().uploads.size()==2,"a changed setting alone changes nothing until the renderer is told");
        vgx::TexturePatchChanged();
        Require(vgx::ResolveTexture(0,c,w,h) && vgx::Pending().uploads.size()==3 && c.texId==b0.texId,"told: the patched texture is uploaded again in place");
        Require(vgx::Pending().uploads.back().levels[0][0]==99,"and carries the new picture");
        Require(vgx::ResolveTexture(1,c,w,h) && vgx::Pending().uploads.size()==3,"a texture the patch does not handle is not touched");
        Require(vgx::ResolveTexture(0,c,w,h) && vgx::Pending().uploads.size()==3,"and the picture is made again once, not at every use");
        std::puts("PASS: a patched texture follows a changed setting (TexturePatchChanged), others are left alone");
        return 0;
    }
    const bool patch=mode=="legacy" || mode=="accept", mip=mode=="mip";
    const bool predicate=mode=="accept" || mode=="reject";
    Require(patch || predicate || mip || mode=="plain","known compact scenario");
    predicateResult=patch;
    vgx::SetTexturePatchHook(patch || predicate?Patch:nullptr,predicate?NeedsRgba:nullptr);
    const std::array<std::array<unsigned,2>,8> dimensions = {{{1,1},{3,5},{7,5},{8,8},{9,9},{16,8},{32,16},{127,65}}};
    for (unsigned fmt=0;fmt<4;++fmt) for (const auto& dim:dimensions) {
        auto bytes=Data(fmt,dim[0],dim[1]);
        expectedHash=XXH3_64bits(bytes.data(),bytes.size()); expectedWidth=dim[0]; expectedHeight=dim[1];
        vgx::ResetState(); vgx::g.bpTexStale=0; vgx::Pending().clear();
        auto& slot=vgx::g.tex[0]; slot.valid=true; slot.avail=bytes.size();
        slot.obj.data=bytes.data(); slot.obj.width=dim[0]; slot.obj.height=dim[1]; slot.obj.format=fmt;
        slot.obj.minFilter=vgx::FILT_LINEAR; slot.obj.magFilter=1; slot.obj.mipmap=mip; slot.obj.maxLod=0;
        vgx::TexBind binding; uint16_t w=0,h=0;
        const auto before=patchCalls, beforePredicate=predicateCalls;
        Require(vgx::ResolveTexture(0,binding,w,h) && vgx::Pending().uploads.size()==1,"real cache queues one upload");
        const auto& up=vgx::Pending().uploads[0];
        const bool compact=vgx::backend::GetCaps().la88 && !mip && !patch;
        const bool called=patch || (predicate && !compact);
        Require(up.format==(compact?vgx::TF_LA88:vgx::TF_RGBA8),"unknown legacy patch hook must fail closed to RGBA");
        Require(patchCalls==before+unsigned(called),"RGBA patch executes exactly once and compact uploads skip it");
        Require(predicateCalls==beforePredicate+unsigned(predicate && vgx::backend::GetCaps().la88 && !mip),
                "eligibility queried only for a new compact candidate");
        if (called) Require(patchHash==expectedHash,"patch sees the same current-content hash as eligibility");
        Check(up,fmt,called);
        const auto id=binding.texId;
        Require(vgx::ResolveTexture(0,binding,w,h) && binding.texId==id && vgx::Pending().uploads.size()==1 &&
                patchCalls==before+unsigned(called),"unchanged cache hit performs no decode/upload/patch");
    }
    std::printf("PASS: 32 real texture-cache compact scenarios mode=%s (not Vita FPS)\n",argv[1]);
}
