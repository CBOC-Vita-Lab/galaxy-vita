#include "port/input.h"
#include "revolution/kpad.h"
#include <algorithm>
#include <cassert>
#include <cmath>
using f32 = float;
using s32 = int;
extern "C" void port_input_use_tilt(float);
struct TVec2f {
    float x,y;
    TVec2f(float a,float b):x(a),y(b){}
    bool isZero() const {return x==0 && y==0;}
    float dot(const TVec2f& o)const{return x*o.x+y*o.y;}
};
struct TVec3f {float x=0,y=0,z=0; TVec3f()=default; TVec3f(float a,float b,float c):x(a),y(b),z(c){};};
namespace MR {
float abs(float x){return std::fabs(x);}
float clamp(float x,float lo,float hi){return std::clamp(x,lo,hi);}
float asin(float x){return std::asin(clamp(x,-1,1));}
float acos(float x){return std::acos(clamp(x,-1,1));}
float sin(float x){return std::sin(x);}
float cos(float x){return std::cos(x);}
float toRadian(float x){return x*3.14159265f/180;}
void normalizeOrZero(TVec2f* v){float n=std::hypot(v->x,v->y); if(n){v->x/=n;v->y/=n;}}
float getInterpolateValue(float t,float a,float b){return a+(b-a)*t;}
void setBckBlendWeight(float,float,float){}
void getCorePadAcceleration(TVec3f* v,int){
    KPADStatus k{}; assert(KPADRead(0,&k,1)==1);
    // Actual WPadAcceleration::update axis transform, consumed below by the
    // verbatim SphereAccelSensorController and SurfRay methods.
    *v={-k.acc.x,k.acc.z,-k.acc.y};
}
void getSubPadAcceleration(TVec3f*,int){assert(false);}
}
struct SphereAccelSensorController {
    enum {Pad_Core,Pad_Sub}; int mPad=Pad_Core;
    bool isCorePad()const{return true;}
    void getPadAcceleration(TVec3f*)const;
    void clacXY(float*,float*);
};
struct TRot3f {
    void identity(){} void setRotate(const TVec3f&,float){} void mult(const TVec3f&,TVec3f&){}
};
struct SurfRay {
    int mPadChannel=0,mTwistBufferSize=1;
    float mRayTilt=0,mSteerAccel=0,mSteerRate=0,mTwistBuffer[1]{};
    TVec3f mRotation,mBaseUp,mFront;
    bool isRotateStart()const{return true;}
    void updateRotate();
};
#include "tilt_consumer_actual.inc"

void test_tilt_consumers() {
    for (float x : {-.65f,0.f,.65f}) for (float z : {-.65f,0.f,.65f}) {
        PortPadState p{}; p.connected=p.tilted=1;
        p.downX=x; p.downZ=z; p.downY=-std::sqrt(1-x*x-z*z);
        port_input_set(0,&p);
        SphereAccelSensorController sphere;
        float sx,sy; sphere.clacXY(&sx,&sy);
        assert(std::isfinite(sx) && std::isfinite(sy));
        assert(x==0 ? std::fabs(sx)<1e-5f : sx*x>0);
        assert(z==0 ? std::fabs(sy)<1e-5f : sy*z>0);
        assert(std::fabs(sx)<=4 && std::fabs(sy)<=4);
        SurfRay ray; ray.updateRotate();
        assert(std::isfinite(ray.mSteerAccel) && std::fabs(ray.mSteerAccel)<=1.5f);
        assert(x==0 ? ray.mSteerAccel==0 : ray.mSteerAccel*x>0);
        assert(x==0 ? ray.mRayTilt==0 : ray.mRayTilt*x>0);
    }
}
