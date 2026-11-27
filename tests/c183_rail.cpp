// Link the original and current rail/rider routines separately. Only stage
// loading and the point-ID table are supplied by the fixture.
#include "Game/LiveActor/RailRider.hpp"
#include "Game/Util/JMapInfo.hpp"
#include "Game/Util/MathUtil.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int remainders, publishes;
namespace MR {
float mod(float a, float b) { ++remainders; return std::fmod(double(a), double(b)); }
}
JMapInfo::JMapInfo() : mData(nullptr), mName(nullptr) {}
JMapInfo::~JMapInfo() {}
s32 JMapInfo::searchItemInfo(const char*) const { return 0; }
bool JMapInfo::getValueFast(int row, int, s32* out) const {
    ++publishes;
    *out = row + 31;   // Deliberately not the array index.
    return true;
}
BezierRail::BezierRail(const JMapInfoIter& iter, const JMapInfo* info)
    : mIsClosed(true), mPointNum(0), mNumRailParts(0), mRailParts(nullptr),
      mPointCoords(nullptr), mIter(nullptr), mInfo(info) {}
RailRider::RailRider(s32, s32)
    : mBezierRail(nullptr), mCoord(0), mSpeed(0), mIsNotReverse(true),
      mCurPos(0,0,0), mCurDirection(0,0,0), mStartPos(0,0,0), mEndPos(0,0,0), mCurPoint(-1) {}

using Point = std::array<TVec3f, 3>;
int main(int argc, char** argv) {
    assert(argc == 3 || argc == 4);
    std::vector<Point> points;
    if (argc == 4) {
        FILE* f = std::fopen(argv[3], "r");
        assert(f);
        Point p;
        while (std::fscanf(f, "%f%f%f%f%f%f%f%f%f", &p[0].x,&p[0].y,&p[0].z,
            &p[1].x,&p[1].y,&p[1].z, &p[2].x,&p[2].y,&p[2].z) == 9) points.push_back(p);
        std::fclose(f);
    } else {
        for (int i = 0; i < 12; ++i) {
            float a = float(i) * .52359878f;
            TVec3f p(std::cos(a)*1200, std::sin(2*a)*130, std::sin(a)*1000);
            TVec3f d(-std::sin(a)*100, 30*std::cos(2*a), std::cos(a)*130);
            points.push_back({p, p-d, p+d});
        }
    }
    assert(points.size() >= 3);
    JMapInfo map;
    BezierRail rail(JMapInfoIter(&map,0), &map);
    std::vector<RailPart> parts(points.size());
    std::vector<float> ends(points.size());
    float acc=0, residual=0;
    for (size_t i=0; i<parts.size(); ++i) {
        auto& p = points[i]; auto& q = points[(i+1) % points.size()];
        parts[i].init(p[0],p[2],q[1],q[0]);
        float partial=residual+parts[i].getTotalLength(), old=acc;
        acc+=partial; residual=partial-(acc-old); ends[i]=acc;
    }
    rail.mPointNum=rail.mNumRailParts=int(parts.size());
    rail.mPointCoords=ends.data(); rail.mRailParts=parts.data();
    RailRider r(0,0);
    r.mBezierRail=&rail;
    FILE* states=std::fopen(argv[1],"wb");
    assert(states);
    // Exercise the wrap function alone including signed zero and reverse endpoints.
    for (float total: {0.f, 1.0f, acc, INFINITY}) {
        ends.back()=total;
        for (int dir: {-1,0,1}) for (float t: {0.f,-0.f,0.00001f,-0.00001f,acc,acc-1,acc+1,-acc,2*acc,100000.f,INFINITY,-INFINITY,NAN}) {
            float result=rail.normalizePos(t,dir);
            // NaN payload/sign is not a serialized game state.
            uint32_t bits=std::isnan(result)?0x7fc00000:std::bit_cast<uint32_t>(result);
            assert(std::fwrite(&bits,4,1,states)==1);
        }
    }
    ends.back()=acc;
    auto step=[&](float t) {
#if BASELINE
        r.setCoord(t); r.move();
#else
        r.moveFromCoord(t);
#endif
    };
    for (bool closed: {false,true}) for (bool forward: {false,true}) {
        rail.mIsClosed=closed; r.mIsNotReverse=forward;
        rail.mNumRailParts=closed?points.size():points.size()-1;
        float length=rail.getTotalLength();
        for (int i=0; i<1900; ++i) {
            float t = i<8?std::array<float,8>{0.f,-0.f,1.e-4f,-1.e-4f,length,-length,2*length,ends[1]}[i]:float(i-310)*length/479.f;
            r.mSpeed=std::array<float,5>{0,7,2.5f,-1,0.001f}[i%5];
            step(t);
            std::array<float,7> s{r.mCoord,r.mCurPos.x,r.mCurPos.y,r.mCurPos.z,r.mCurDirection.x,r.mCurDirection.y,r.mCurDirection.z};
            assert(std::fwrite(s.data(),sizeof(float),s.size(),states)==s.size());
            assert(std::fwrite(&r.mCurPoint,sizeof(r.mCurPoint),1,states)==1);
        }
    }
    std::fclose(states);
    rail.mIsClosed=r.mIsNotReverse=true; rail.mNumRailParts=points.size(); r.mSpeed=0;
    int rounds=std::atoi(argv[2]);
    std::vector<double> timings;
    int count=24000;
    for (int j=0;j<rounds;++j) {
        publishes=remainders=0; float t=0;
        auto start=std::chrono::steady_clock::now();
        for (int i=0;i<count;++i) { step(t-800.f); t+=6.7f; if(t>=acc) t-=acc; }
        timings.push_back(std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/count);
    }
    std::sort(timings.begin(),timings.end());
    std::printf("%s: %zu points, 7600 full states, %d steps/run; %d publishes, %d remainders; %.1f ns/step host median (%d samples)\n",
                BASELINE?"C182":"C183",points.size(),count,publishes,remainders,timings[rounds/2],rounds);
    for (auto& p: parts) { delete p.mRailPartLinear; delete p.mRailPartBezier; }
}
