// Differential test of the NW4R quad/glyph stream path against the ordinary decoder.
#include "../platform/vgx/src/vgx_state.cpp"
#include <array>
#include <chrono>

namespace vgx {
bool LogEnabled() { return false; }
}

static bool Particles(bool enabled, bool bench) {
    using namespace vgx;
    uint32_t draws = 0;
    uint8_t fmtDesc[26]{};
    fmtDesc[A_POS] = fmtDesc[A_TEX0] = T_INDEX8;
    VtxAttrFmt fmt[8][26]{};
    fmt[0][A_POS] = fmt[0][A_TEX0] = {1,C_S8,0};
    PlanCache cache{};
    bool dirty = true;
    const Plan& base = GetPlanIn(cache,dirty,fmtDesc,fmt,0,false);
    if (!!base.quad != enabled) return false;
    std::array<uint8_t,32> pos{}, st{}, posOther{};
    for (unsigned i=0;i<32;i++) {pos[i]=31*i; st[i]=255-53*i; posOther[i]=128+i;}
    const uint8_t usual[] = {0,0,1,1,2,2,3,3};
    for (bool folded : {false,true}) {
        const Plan& p = folded ? GetFoldedPlanIn(cache,0,base) : base;
        for (int variant=0;variant<12;variant++) {
            g.arrays[A_POS] = {pos.data(),12,3,false};
            g.arrays[A_TEX0] = {st.data(),8,2,false};
            uint8_t idx[8]; std::memcpy(idx,usual,8);
            switch (variant) {
                case 1: idx[6]=2; break;
                case 2: idx[4]=250; break;
                case 3: g.arrays[A_POS].data=nullptr; break;
                case 4: g.arrays[A_TEX0].size=7; break;
                case 5: g.arrays[A_POS].stride=4; break;
                case 6: g.arrays[A_TEX0].stride=0; break;
                case 7: g.arrays[A_POS].stride=0xFFFFFFFFu; break;
                case 8: g.arrays[A_POS].le=true; g.arrays[A_TEX0].le=true; break;
                case 9: pos[3]^=255; st[4]^=0x80; break;
                case 10: g.arrays[A_POS].data=posOther.data(); break;
                case 11: g.arrays[A_POS].size=0; break;
            }
            for (unsigned count : {1u,4u}) for (bool spans : {false,true}) {
                std::vector<uint8_t> dst(7+count*p.layout.stride+1,0xA5), ref=dst;
                ArraySpans a{},b{};
                DecodeVertices(p,idx,count,dst.data()+7,spans?&a:nullptr);
                const auto max=LastDecodedPosMtxMax();
                DecodeVerticesGeneral(p,idx,count,ref.data()+7,spans?&b:nullptr,false);
                if (dst!=ref || max!=LastDecodedPosMtxMax() || a.mask!=b.mask ||
                    std::memcmp(a.lo,b.lo,sizeof a.lo) || std::memcmp(a.hi,b.hi,sizeof a.hi)) return false;
                ++draws;
            }
        }
    }
    if (bench && base.quad) {
        Plan old=base; old.quad=nullptr;
        g.arrays[A_POS]={pos.data(),12,3,false};
        g.arrays[A_TEX0]={st.data(),8,2,false};
        std::vector<uint8_t> dst(4*base.layout.stride);
        auto time=[&](const Plan& p) {
            const auto t=std::chrono::steady_clock::now();
            for(int i=0;i<500000;i++) {
                DecodeVerticesFast(p,usual,4,dst.data(),nullptr);
                asm volatile("" : : "r"(dst.data()) : "memory");
            }
            return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-t).count()/500000;
        };
        std::array<double,9> a{},b{};
        for (int i=0;i<9;i++) {
            if(i%2){a[i]=time(old); b[i]=time(base);} else {b[i]=time(base); a[i]=time(old);}
        }
        std::sort(a.begin(),a.end()); std::sort(b.begin(),b.end());
        std::printf("JPA CPU host-only old_ns=%.2f fused_ns=%.2f\n",a[4],b[4]);
    }
    g.arrays[A_POS]={}; g.arrays[A_TEX0]={};
    std::printf("particle decode: %u guarded differential cases passed\n",draws);
    return true;
}
int main(int argc, char**) {
    using namespace vgx;
    const bool bench = argc > 1;
    const bool enabled = EnvInt("GXV_QUAD_DECODE", 1) && PositionDefaultsMode() != 2;
    size_t cases = 0;
    uint32_t rng = 42;
    for (int xyz : {0,1}) for (int clr : {0,1}) for (int tex : {C_F32,C_U16})
    for (int frac : {0,7,15,31}) for (int extra = 0; extra < 9; ++extra) {
        PlanCache cache{};
        uint8_t desc[26]{};
        VtxAttrFmt fmt[8][26]{};
        desc[A_POS] = desc[A_TEX0] = T_DIRECT;
        if (clr) desc[A_CLR0] = T_DIRECT;
        fmt[0][A_POS] = {static_cast<uint8_t>(xyz), C_F32, 0};
        fmt[0][A_TEX0] = {1, static_cast<uint8_t>(tex), static_cast<uint8_t>(frac)};
        fmt[0][A_CLR0] = {1, 5, 0};
        switch (extra) {
            case 1: desc[A_TEX0 + 1] = T_DIRECT; break;
            case 2: desc[A_PNMTXIDX] = T_DIRECT; break;
            case 3: desc[A_NRM] = T_DIRECT; break;
            case 4: desc[A_POS] = T_INDEX8; break;
            case 5: fmt[0][A_TEX0].cnt = 0; break;
            case 6: desc[A_CLR0] = T_DIRECT; fmt[0][A_CLR0].type = 3; break;
            case 7: desc[A_TEX0MTXIDX] = T_DIRECT; break;
            case 8: fmt[0][A_POS].type = C_S16; break;
        }
        bool dirty = true;
        const Plan& base = GetPlanIn(cache, dirty, desc, fmt, 0, false);
        for (bool folded : {false,true}) {
            const Plan& p = folded ? GetFoldedPlanIn(cache, 0, base) : base;
            if ((p.quad != nullptr) != (enabled && extra == 0)) return 1;
            std::vector<uint8_t> src(4*p.streamSize+1);
            for (uint8_t& b : src) { rng = 1664525u*rng + 1013904223u; b = rng >> 24; }
            if (!extra) { // +/-0, sNaN, subnormal, infinity; endian swap without FP interpretation
                static constexpr uint8_t edge[] = {0x80,0,0,0,0x7f,0x80,0,1};
                std::memcpy(src.data()+1, edge, sizeof edge);
            }
            for (unsigned count : {0u,1u,3u,4u}) {
                std::vector<uint8_t> actual(3+count*p.layout.stride+7,0xA5), expect = actual;
                ArraySpans span{}, spanRef{};
                DecodeVertices(p, src.data()+1, count, actual.data()+3, &span);
                const auto max = LastDecodedPosMtxMax();
                DecodeVerticesGeneral(p, src.data()+1, count, expect.data()+3, &spanRef, false);
                if (actual != expect || max != LastDecodedPosMtxMax() || span.mask != spanRef.mask ||
                    std::memcmp(span.lo, spanRef.lo, sizeof span.lo) || std::memcmp(span.hi, spanRef.hi, sizeof span.hi)) return 2;
                ++cases;
            }
            if (!bench || folded || extra || frac != 15 || !p.quad) continue;
            Plan old = p;
            old.quad = nullptr;
            std::vector<uint8_t> dst(4*p.layout.stride);
            const auto time = [&](const Plan& plan) {
                const auto t = std::chrono::steady_clock::now();
                for (unsigned i=0; i<500000; ++i) {
                    DecodeVerticesDirect(plan,src.data()+1,4,dst.data());
                    asm volatile("" : : "r"(dst.data()) : "memory");
                }
                return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-t).count()/500000;
            };
            std::array<double,9> a{}, b{};
            for (int i=0;i<9;++i) {
                if (i%2) { a[i]=time(old); b[i]=time(p); }
                else { b[i]=time(p); a[i]=time(old); }
            }
            std::sort(a.begin(),a.end()); std::sort(b.begin(),b.end());
            std::printf("quad CPU host-only pos=%d color=%d tex=%s old_ns=%.2f fused_ns=%.2f\n",
                        2+xyz,clr,tex==C_F32?"f32":"u16",a[4],b[4]);
        }
    }
    std::printf("quad decode: %zu guarded differential cases passed; recipes %s\n", cases, enabled?"enabled":"disabled");
    return Particles(enabled,bench)?0:3;
}
