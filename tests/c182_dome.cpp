#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
using f32=float; using s32=int; using u32=unsigned;
struct TVec3f {
    float x{},y{},z{};
    TVec3f()=default; TVec3f(float x,float y,float z):x(x),y(y),z(z){}
    void negate(const TVec3f& a){x=-a.x;y=-a.y;z=-a.z;}
    void zero(){x=y=z=0;}
    void add(const TVec3f& a){x+=a.x;y+=a.y;z+=a.z;}
    void set(const TVec3f& a){*this=a;}
    template<class T> void set(T a,T b,T c){x=a;y=b;z=c;}
    void cross(const TVec3f& a,const TVec3f& b){*this={a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
};
using MtxPtr = float (*)[4];
struct StarPointerTarget { TVec3f mOffset{}; const TVec3f* mPosition=nullptr; MtxPtr mMtx=nullptr; void calcPosition(TVec3f*) const; };
constexpr unsigned ACTMES_SPHERE_SELECTOR_SELECT_START=9;
enum {cRotateAppearX,cBgmAppearState,cBgmAppearFrames,cZoomFrame,MiniatureGalaxyState_Open,MiniatureGalaxyState_New,MiniatureGalaxyState_Hatena,MiniatureGalaxyState_Unknown};
constexpr float cPointingRadius=2500,cZoomOutScale=.65f;
#define GET_NERVE(a,b) 0
struct LiveActor {
    TVec3f mPosition{880,460,-22000},mRotation{}; float base[3][4]{}; bool dead=true,model=true; int opens=0, deliveries=0;
    virtual void appear(){++opens;dead=false;base[0][3]=mPosition.x;base[1][3]=mPosition.y;base[2][3]=mPosition.z;}
    void setNerve(int){} MtxPtr getBaseMtx(){return model?base:nullptr;}
};
struct SphereSelectorHandle:LiveActor {
    bool mIsFileSelectMode=false; TVec3f _C0,_E8,_DC; int _10C=0; int published=0;
    void updateBaseMtx(){++published;} void resetRotateParam(){} void clearPointerVelocity(){} void appear() override;
};
struct Orbit:LiveActor {
    SphereSelectorHandle* h=nullptr; TVec3f desired;
    void calcGalaxyPos(TVec3f* p){*p=h->published?desired:TVec3f{-999,-999,-999};}
};
struct MiniatureGalaxy:LiveActor {
    Orbit* mOrbit; LiveActor *mShadowModel,*mSelectModel; StarPointerTarget pick;
    int mCanZoomIn=0,mZoomLevel=0,mState=0; const char *mObjectName="",*mGalaxyName="";
    void setScale(float){} void showSelectModel(){} bool isUseKoopaFaceModel()const{return false;}
    void appearUnknownModel(const char*){} void appear() override; void initPick();
};
struct Group {std::vector<LiveActor*> targets; int getObjNum(){return targets.size();} LiveActor* getActor(int i){return targets[i];}};
struct SphereSelector {Group* mSphereGroup; SphereSelectorHandle* mHandle; void sendMsgToAllActor(u32);};
namespace MiniatureGalaxyFunction {static int calcMiniatureGalaxyIndex(MiniatureGalaxy*){return 1;}}
static void setInitialRotation(TVec3f&,float){}
namespace MR {
static TVec3f z{0,0,1},up{0,1,0};
static TVec3f getCamZdir(){return z;} static TVec3f getCamYdir(){return up;}
static void normalize(TVec3f* d){float l=std::sqrt(d->x*d->x+d->y*d->y+d->z*d->z);d->x/=l;d->y/=l;d->z/=l;}
static bool normalizeOrZero(TVec3f* d){if(d->x*d->x+d->y*d->y+d->z*d->z<1e-8f){d->zero();return true;}normalize(d);return false;}
static void rotateVecDegree(TVec3f*d,const TVec3f&a,const TVec3f&,float){*d=a;}
static void setStageBGMState(int,int){} static bool isAppearGalaxy(const char*){return true;}
static bool isOnGameEventFlagGalaxyOpen(const char*){return true;} static bool canOpenGalaxy(const char*){return true;}
static void showModelIfHidden(LiveActor*){} static void emitEffect(LiveActor*,const char*){}
static void tryStartAllAnim(LiveActor*,const char*){} static void startBtk(LiveActor*,const char*){}
static void sendSimpleMsgToActor(u32 msg,LiveActor* a){++a->deliveries;if(msg==ACTMES_SPHERE_SELECTOR_SELECT_START)a->appear();}
static void initStarPointerTarget(MiniatureGalaxy*a,float,const TVec3f&){a->pick.mPosition=&a->mPosition;a->pick.mMtx=a->getBaseMtx();}
static void initStarPointerTargetAtMtx(MiniatureGalaxy*a,float,MtxPtr m,const TVec3f&){a->pick.mMtx=m;}
}
void MiniatureGalaxy::initPick(){
#include "target-init.inc"
}
#include "select.inc"
struct StarPointerTransformHolder {
    template<int Rows> struct Mat {
        float v[Rows][4]; explicit Mat(MtxPtr src){std::memcpy(v,src,sizeof(v));}
    };
    Mat<3> mViewMtx; Mat<4> mProjMtx; float mFovy,mFocalLength;
    StarPointerTransformHolder();
};
#include "holder.inc"
int main() {
    SphereSelectorHandle h; LiveActor shadow,select;
    Orbit o; o.h=&h; o.desired={1200,400,-18000}; MiniatureGalaxy a; a.mOrbit=&o;a.mShadowModel=&shadow;a.mSelectModel=&select;
    a.initPick();
    Group g{{&a,&h}}; SphereSelector s{&g,&h};
    for (int i=0;i<2;++i) {
        h.published=0; h.dead=true; o.desired.x+=300; s.sendMsgToAllActor(ACTMES_SPHERE_SELECTOR_SELECT_START);
        assert(a.opens==i+1 && h.opens==i+1 && h.published && a.base[0][3]==o.desired.x);
        a.mPosition.x+=1000; // the actor advances before a budgeted model's next pose
        TVec3f picked; a.pick.calcPosition(&picked); assert(picked.x==a.base[0][3] && picked.y==a.base[1][3]);
    }
    MR::z={0,1,0}; MR::up={1,0,0}; h.appear(); assert(std::isfinite(h._C0.x) && std::isfinite(h._C0.z));
    MiniatureGalaxy missing; missing.model=false; missing.initPick(); TVec3f fallback; missing.pick.calcPosition(&fallback);
    assert(fallback.x==missing.mPosition.x && fallback.y==missing.mPosition.y && fallback.z==missing.mPosition.z);
    StarPointerTransformHolder t; assert(t.mProjMtx.v[3][3]==1 && t.mFovy>0 && std::isfinite(t.mFocalLength));
    puts("PASS: first open and reopen publish orbit before model; picking follows displayed transform; vertical entrance and pointer initialization");
}
