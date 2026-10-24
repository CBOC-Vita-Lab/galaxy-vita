// Only kernel/GXM driver calls are doubled; ownership/upload/sampler code is real.
#define main device_fixture_not_run
#include "vita_gpu_device.cpp"
#undef main

#pragma weak port_vita_gxm_sampler_decode
#pragma weak port_vita_gxm_texture_view_create
#pragma weak port_vita_gxm_texture_view_info
#pragma weak port_vita_gxm_texture_view_descriptor
#pragma weak port_vita_gxm_texture_view_release
extern "C" int port_vita_gxm_texture_view_create_explicit_linear_mip(PortVitaGxmTexture*,uint32_t,uint32_t,PortVitaGxmTextureView**);
#pragma weak port_vita_gxm_texture_view_create_explicit_linear_mip

int main() {
    require(port_vita_gxm_sampler_decode && port_vita_gxm_texture_view_create &&
            port_vita_gxm_texture_view_info && port_vita_gxm_texture_view_descriptor &&
            port_vita_gxm_texture_view_release,"GX immutable sampler views exist");
    PortVitaGxmSampler sampler{};
    const uint32_t advanced=1u|(2u<<2)|(6u<<5)|(1u<<4)|(128u<<9)|(1u<<8)|
        (1u<<17)|(1u<<18)|(2u<<19)|(1u<<21);
    require(port_vita_gxm_sampler_decode(advanced,0x2723,&sampler)==0 && sampler.mode0==advanced &&
        sampler.mode1==0x2723 && sampler.wrap_s==1 && sampler.wrap_t==2 && sampler.min_filter==6 &&
        sampler.mag_filter==1 && sampler.mip_filter==2 && sampler.bias_units==-128 && sampler.bias==-4 &&
        sampler.min_units==35 && sampler.max_units==39 && sampler.min_lod==2.1875f && sampler.max_lod==2.4375f &&
        !sampler.edge_lod && sampler.round_lod && sampler.field_predict && sampler.anisotropy==4 && sampler.bias_clamp,
        "all raw advanced fields and fractional units retained without native quantization");
    require(port_vita_gxm_sampler_decode(127u<<9,0,&sampler)==0 && sampler.bias==3.96875f,
            "signed positive GX bias endpoint is exact");
    const uint32_t invalid_modes[]={3,12,3u<<5,7u<<5,3u<<19,1u<<22,1u<<24};
    for (uint32_t mode:invalid_modes) {
        sampler.mode0=7;
        require(port_vita_gxm_sampler_decode(mode,0,&sampler)<0 && !sampler.mode0,
                "reserved BP sampler bits reject transactionally rather than defaulting");
    }
    require(port_vita_gxm_sampler_decode(0,0x10000,&sampler)<0,"reserved mode1 bits reject");
    PortVitaGxmDevice* device=nullptr; PortVitaGxmTexture* texture=nullptr;
    require(port_vita_gxm_device_create(&device)==0,"sampler fixture native device");
    std::vector<uint32_t> rgba(64+16+4+1,0x7F123456);
    require(port_vita_gxm_texture_create(device,8,8,4,77,rgba.data(),rgba.size()*4,&texture)==0,
            "sampler fixture full chain");
    PortVitaGxmTextureInfo image{}; require(port_vita_gxm_texture_info(texture,&image)==0,"image identity");
    const void* original=nullptr; require(port_vita_gxm_texture_descriptor(texture,&original)==0,"diagnostic descriptor");
    const auto original_descriptor=*static_cast<const SceGxmTexture*>(original);
    const unsigned allocations=memory.size();
    require(port_vita_gxm_texture_view_create_explicit_linear_mip,
            "explicit diagonal linear-mip sampling has a separate view contract, not an implicit fallback");
    PortVitaGxmTextureView* explicitView=nullptr;
    require(port_vita_gxm_texture_view_create(texture,0x1d5,0x3000,&explicitView)<0 && !explicitView,
            "the observed diagonal mip state remains rejected by the implicit shader API");
    require(port_vita_gxm_texture_view_create_explicit_linear_mip(texture,0x1d5,0x2f01,&explicitView)==0,
            "explicit shader sampler retains fractional original LOD bounds without native rounding");
    PortVitaGxmTextureViewInfo explicitInfo{}; const void* explicitDescriptor=nullptr;
    require(port_vita_gxm_texture_view_info(explicitView,&explicitInfo)==0 &&
            explicitInfo.explicit_linear_mip==1 &&
            explicitInfo.sampler.mode0==0x1d5 && explicitInfo.sampler.mode1==0x2f01 &&
            explicitInfo.sampler.min_units==1 && explicitInfo.sampler.max_units==47 &&
            explicitInfo.image.pixels==image.pixels && explicitInfo.image.levels==4 &&
            explicitInfo.native_min_lod==0 && explicitInfo.native_max_lod==3 &&
            port_vita_gxm_texture_view_descriptor(explicitView,device,&explicitDescriptor)==0,
            "shader-selected LOD can read every original mip without reupload or losing raw sampler provenance");
    const auto* explicitNative=static_cast<const SceGxmTexture*>(explicitDescriptor);
    require(explicitNative->generic.min_filter==3 && explicitNative->generic.mag_filter==0 &&
            explicitNative->generic.mip_filter==0 && explicitNative->generic.mip_count==3 &&
            explicitNative->generic.lod_bias==31 && explicitNative->lod_min0==0 &&
            explicitNative->generic.uaddr_mode==2 && explicitNative->generic.vaddr_mode==2,
            "explicit point fetches clamp natively; original repeat/mirror belongs to shader corner wrapping, not unsupported linear-GXM mirror");
    require(memory.size()==allocations && std::memcmp(original,&original_descriptor,sizeof(original_descriptor))==0,
            "explicit sampler leaves immutable image storage and old sampler descriptors unchanged");
    require(port_vita_gxm_texture_view_release(&explicitView)==0 && !explicitView,
            "explicit shader view has the same fence-gated owner lifetime");
    for(uint32_t mode:{0xd5u,0x1c5u,0x1b5u,0x1d5u|(1u<<17),0x1d5u|(1u<<18),
                       0x1d5u|(1u<<19),0x1d5u|(1u<<21)})
        require(port_vita_gxm_texture_view_create_explicit_linear_mip(texture,mode,0x3000,&explicitView)<0 && !explicitView,
                "unproven filter/LOD strategies cannot acquire the explicit linear diagonal contract");
    require(port_vita_gxm_texture_view_create_explicit_linear_mip(texture,0x1d5,0x1020,&explicitView)<0 && !explicitView,
            "explicit sampling cannot conceal an inverted original LOD interval");
    for(unsigned i=1;i<=8;++i) {
        sampler_fail_call=sampler_calls+i;
        require(port_vita_gxm_texture_view_create_explicit_linear_mip(texture,0x1d5,0x3000,&explicitView)==-861 &&
                !explicitView && memory.size()==allocations,
                "explicit sampler setter failures retain no partial view or extra image");
        sampler_fail_call=0;
    }
    texture_error=-863;
    require(port_vita_gxm_texture_view_create_explicit_linear_mip(texture,0x1d5,0x3000,&explicitView)==-863 && !explicitView,
            "explicit native texture validation failure remains transactional");
    texture_error=0;
    // Literals derived from GX hardware min-filter bits and SDK enum values.
    const uint32_t gx[]={0,4,1,2,5,6}, native[]={0,1,3,3,2,2}, mip[]={0,0,0,1,0,1};
    const uint32_t address[]={2,0,1};
    PortVitaGxmTextureView* views[6]{};
    for (unsigned i=0;i<6;++i) {
        const uint32_t mode=(gx[i]<<5)|(i%3)|(((i+1)%3)<<2)|((i%2)<<4)|(255u<<9);
        require(port_vita_gxm_texture_view_create(texture,mode,0x3010,&views[i])==0,"all six literal GX filter mappings");
        const void* descriptor=nullptr;
        require(port_vita_gxm_texture_view_descriptor(views[i],device,&descriptor)==0 && descriptor!=original,
                "per-draw descriptor does not alias mutable default sampler");
        const auto* t=static_cast<const SceGxmTexture*>(descriptor);
        require(t->generic.min_filter==native[i] && t->generic.mip_filter==mip[i] &&
                t->generic.mag_filter==i%2 && t->generic.uaddr_mode==address[i%3] &&
                t->generic.vaddr_mode==address[(i+1)%3] && t->generic.lod_bias==31 &&
                t->generic.mip_count==(i<2 ? 0u:3u) && t->lod_min0==(i<2 ? 0u:1u),
                "actual SDK-bound descriptor has independent filters/wraps/integer range and neutral bias");
        PortVitaGxmTextureViewInfo info{};
        require(port_vita_gxm_texture_view_info(views[i],&info)==0 && info.image.pixels==image.pixels &&
                info.image.version==77 && info.sampler.mode0==mode && info.sampler.bias==-0.03125f &&
                info.native_min_lod==(i<2 ? 0u:1u) && info.native_max_lod==(i<2 ? 0u:3u),
                "immutable views share versioned pixels but retain exact original GX state");
    }
    require(memory.size()==allocations && std::memcmp(original,&original_descriptor,sizeof(original_descriptor))==0,
            "sampler variants neither reupload pixels nor mutate old descriptors");
    for (uint32_t mode:{0x105u,0x195u}) {
        PortVitaGxmTextureView* base=nullptr;
        require(port_vita_gxm_texture_view_create(texture,mode,0x3000,&base)==0 && base,
                "diagonal LOD cannot alter a nonmip base sample with matching minification and magnification filters");
        PortVitaGxmTextureViewInfo info{}; const void* descriptor=nullptr;
        require(port_vita_gxm_texture_view_info(base,&info)==0 && !info.sampler.edge_lod &&
                info.sampler.mode0==mode && info.sampler.mode1==0x3000 &&
                info.image.levels==4 && info.image.storage_bytes==480 &&
                info.native_min_lod==0 && info.native_max_lod==0 &&
                port_vita_gxm_texture_view_descriptor(base,device,&descriptor)==0,
                "exact diagonal flag and complete original mip storage survive the base-only sampler view");
        const auto* native_base=static_cast<const SceGxmTexture*>(descriptor);
        const unsigned filter=mode==0x105 ? 0 : 1;
        require(native_base->generic.min_filter==filter && native_base->generic.mag_filter==filter &&
                native_base->generic.mip_count==0 && native_base->generic.mip_filter==0 &&
                native_base->generic.uaddr_mode==0 && native_base->generic.vaddr_mode==0,
                "the actual SDK descriptor uses original equal point/linear filters and repeat wrapping without mips");
        require(port_vita_gxm_texture_view_release(&base)==0 && !base,
                "base-only diagonal sampler retires its native view normally");
    }
    for (uint32_t mode:{0x115u,0x185u}) {
        PortVitaGxmTextureView* mixed=nullptr;
        require(port_vita_gxm_texture_view_create(texture,mode,0x3000,&mixed)<0 && !mixed,
                "diagonal LOD still requires an exact strategy when minification and magnification filters differ");
    }
    require(port_vita_gxm_texture_release(&texture)<0 && texture,"views keep parent GPU image alive");
    PortVitaGxmTextureView* bad=nullptr;
    const uint32_t unsupported[]={1u<<8,1u<<17,1u<<18,1u<<19,1u<<21};
    for (uint32_t flag:unsupported) require(port_vita_gxm_texture_view_create(texture,(6u<<5)|flag,0x3000,&bad)<0 && !bad,
            "advanced LOD strategies must not silently use basic native filtering");
    require(port_vita_gxm_texture_view_create(texture,6u<<5,0x2F00,&bad)<0 && !bad &&
            port_vita_gxm_texture_view_create(texture,6u<<5,0x3001,&bad)<0 && !bad,
            "reachable fractional min/max LOD rejects instead of rounding");
    require(port_vita_gxm_texture_view_create(texture,6u<<5,0x1020,&bad)<0 && !bad,
            "inverted GX LOD interval rejects");
    for (unsigned i=1;i<=8;++i) {
        sampler_fail_call=sampler_calls+i;
        require(port_vita_gxm_texture_view_create(texture,6u<<5,0x3000,&bad)==-861 && !bad && memory.size()==allocations,
                "every failed native sampler setter rolls back unpublished view"); sampler_fail_call=0;
    }
    texture_error=-862;
    require(port_vita_gxm_texture_view_create(texture,6u<<5,0x3000,&bad)==-862 && !bad,
            "native sampler validation failure remains transactional"); texture_error=0;
    require(port_vita_gxm_device_begin_display(device)==0 &&
            port_vita_gxm_texture_view_create(texture,0,0,&bad)<0 && !bad &&
            port_vita_gxm_texture_view_release(&views[0])<0 && views[0],
            "scene-open creation/release cannot change GPU-visible ownership");
    require(port_vita_gxm_device_end(device,nullptr,0)==0,"sampler fixture scene ended");
    unsigned before=finishes;
    for (auto& view:views) require(port_vita_gxm_texture_view_release(&view)==0 && !view,"view retirement succeeds");
    require(finishes>=before+6 && !pending_gpu,"view release fenced before descriptor/parent retirement");
    require(port_vita_gxm_texture_release(&texture)==0,"unleased image released");
    std::vector<uint32_t> tail(16+4+2+1,0xAABBCCDD);
    require(port_vita_gxm_texture_create(device,8,2,4,78,tail.data(),tail.size()*4,&texture)==0,
            "rectangular tail fixture");
    require(port_vita_gxm_texture_view_create_explicit_linear_mip(texture,0x1d5,0x3000,&explicitView)<0 && !explicitView,
            "manual square-mip contract cannot silently drop rectangular native mip tails");
    require(port_vita_gxm_texture_view_create(texture,6u<<5,0x3000,&bad)<0 && !bad,
            "reachable rectangular tail is never dropped by native mip count");
    require(port_vita_gxm_texture_view_create(texture,6u<<5,0x1000,&bad)==0,
            "GX maxLOD explicitly excludes tail, not a quality reduction");
    require(port_vita_gxm_texture_view_release(&bad)==0 &&
            port_vita_gxm_texture_view_create(texture,4u<<5,0x3000,&bad)==0,
            "nonmip GX filtering legitimately samples base while full tail remains stored");
    require(port_vita_gxm_texture_view_release(&bad)==0 && port_vita_gxm_texture_release(&texture)==0 &&
            port_vita_gxm_device_release(&device)==0,"sampler resources released without GPU leaks");
    empty_state();
    printf("Vita texture views: %u sanitized real ownership/sampler checks; no native pixel proof\n",checks);
}
