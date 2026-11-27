// Exercise the production method with a renderer that consumes the normal per-picture stamp.
#include <cassert>
#include <cstdint>
#include <cstdio>
using u32 = uint32_t;
using s32 = int;
using f32 = float;
struct TVec3f {
    float x = 0, y = 0, z = 0;
    TVec3f operator-(const TVec3f& b) const { return {x-b.x, y-b.y, z-b.z}; }
    float squared() const { return x*x+y*y+z*z; }
};
struct J3DModel { u32 mPortCullPicture = 0; };
struct PlantMember {
    bool dead = false, hidden = false, clipped = false, mHasItem = false;
    int mDetailThin = 0, mDetailTgt = 0;
    J3DModel model;
    J3DModel* visibleModel = &model;
    TVec3f mPosition;
};
namespace MR {
bool isDead(const PlantMember* m) { return m->dead; }
bool isHiddenModel(const PlantMember* m) { return m->hidden; }
bool isClipped(const PlantMember* m) { return m->clipped; }
J3DModel* getJ3DModel(PlantMember* m) { return m->visibleModel; }
}
static bool perf = true;
static TVec3f far{3500,0,0}, near{899,0,0};
static const TVec3f* player = &far;
int port_pb_budget_on() { return perf; }
const TVec3f* port_safe_player_pos() { return player; }
// (fix31) a GXV_PB_DEMO=2 flight, the popin31 floor / release guard, and the census
static int demoOff = 0, guardOn = 0, logOn = 0, events = 0, eventKind = -1;
static const PlantMember* lastEvent = nullptr;
int port_pb_demo_off() { return demoOff; }
namespace port { namespace popin {
enum { kPlantReveal = 10 };
enum { kFlDecor = 1 };   // (popin31b: the plant groups' floor kind; the production site asks GuardOn(kFlDecor))
inline bool GuardOn(int fk) { return fk == kFlDecor && guardOn != 0; }
inline int LogOn() { return logOn; }
} }
void port_popin_event(int kind, int appear, const PlantMember* m, const TVec3f* pos, float) { assert(appear == 1 && pos == &m->mPosition); ++events; eventKind = kind; lastEvent = m; }
struct PlantGroup {
    bool mPortSplitNeutral = true;
    int mMemberCount = 7;
    float mPortHrRadius = 0;
    TVec3f mPosition;
    PlantMember storage[8];
    PlantMember* mMembers[8]{};
    PlantGroup() { for (int i = 0; i < 8; i++) mMembers[i] = &storage[i]; }
    void portSplitDecorReveal(const TVec3f&, u32);
    unsigned visible(u32 tick) const {
        unsigned mask = 0;
        for (int i = 0; i < mMemberCount; i++) {
            const auto& m = storage[i];
            if (!m.dead && !m.hidden && !m.clipped && m.visibleModel && m.model.mPortCullPicture != tick) mask |= 1u << i;
        }
        return mask;
    }
};
#define PORT_LOGGING 0
#include "plant.inc"
int main() {
    constexpr u32 a = 42;
    for (int n = 1; n <= 8; n++) {
        PlantGroup g;
        g.mMemberCount = n;
        unsigned control = g.visible(a);
        g.portSplitDecorReveal(far,a);
        unsigned first = g.visible(a);
        assert((first & ~control) == 0 && (first & 1));
        assert(__builtin_popcount(first) == ((n >= 3 && n <= 7) ? (n+1)/2 : n));
        assert(g.visible(a) == first && g.visible(a+1) == control && g.visible(a+2) == control);
        if (n == 7) {
            assert(first == 0x55);
            std::printf("Seven live neutral members; native picture visibility masks: control=00/7f/7f candidate=00/%02x/7f\n", first);
        }
    }
    for (int k=0; k<12; k++) {
        PlantGroup g; TVec3f cam = far; u32 tick = a;
        if(k==0) g.mPortSplitNeutral=false;
        if(k==1) perf=false;
        if(k==2) player=nullptr;
        if(k==3) player=&near;
        if(k==4) cam=near;
        if(k==5) tick=0; // the J3D zero sentinel is never written
        if(k==6) g.mPortHrRadius=3000;
        if(k==7) g.storage[6].mHasItem=true;
        if(k==8) g.storage[6].mDetailThin=1;
        if(k==9) g.storage[6].mDetailTgt=1;
        if(k==10) demoOff=1;   // (fix31: a GXV_PB_DEMO=2 flight never staggers: mPbFar was cleared, the whole held set "returns" on its first turn)
        if(k==11) guardOn=1;   // (fix31: nor while the floor / the release after a flight is in force)
        logOn=1; events=0;
        g.portSplitDecorReveal(cam,tick);
        assert(g.visible(a)==0x7f && events==0);
        player=&far; perf=true; demoOff=0; guardOn=0; logOn=0;
    }
    PlantGroup sparse;
    sparse.storage[0].dead=true; sparse.storage[2].hidden=true;
    sparse.storage[3].clipped=true; sparse.storage[4].visibleModel=nullptr;
    sparse.storage[5].model.mPortCullPicture=a;
    assert(sparse.visible(a)==0x42);
    sparse.portSplitDecorReveal(far,a);
    assert(sparse.visible(a)==0x42 && sparse.storage[5].model.mPortCullPicture==a);
    sparse.storage[0].dead=false;
    sparse.portSplitDecorReveal(far,a);
    assert(sparse.visible(a)==0x41 && sparse.visible(a+1)==0x63);
    {   // (fix31) the census counts each member held back one picture as a port-made appearance; nothing when the census is off
        PlantGroup g; logOn=1; events=0;
        g.portSplitDecorReveal(far,a);
        assert(g.visible(a)==0x55 && events==3 && eventKind==port::popin::kPlantReveal && lastEvent==&g.storage[5]);
        PlantGroup h; logOn=0; events=0;
        h.portSplitDecorReveal(far,a);
        assert(h.visible(a)==0x55 && events==0);
        std::puts("fix31: no stagger in a GXV_PB_DEMO=2 flight or under the floor/release guard; the census counts the 3 held members of 7: PASS");
    }
    PlantGroup wrap;
    wrap.portSplitDecorReveal(far,UINT32_MAX);
    assert(wrap.visible(UINT32_MAX)==0x55 && wrap.visible(1)==0x7f);
    std::puts("Sparse, existing cull, near, category, preset, missing player, native picture lifetime: PASS");
}
