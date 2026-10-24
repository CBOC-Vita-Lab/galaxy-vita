#include "Game/Camera/CameraHolder.hpp"
#include "Game/Camera/Camera.hpp"
#include "Game/Camera/CameraAnim.hpp"
#include "Game/Camera/CameraBehind.hpp"
#include "Game/Camera/CameraBlackHole.hpp"
#include "Game/Camera/CameraCharmedFix.hpp"
#include "Game/Camera/CameraCharmedTripodBoss.hpp"
#include "Game/Camera/CameraCharmedVecReg.hpp"
#include "Game/Camera/CameraCharmedVecRegTower.hpp"
#include "Game/Camera/CameraCubePlanet.hpp"
#include "Game/Camera/CameraDPD.hpp"
#include "Game/Camera/CameraDead.hpp"
#include "Game/Camera/CameraFix.hpp"
#include "Game/Camera/CameraFixedPoint.hpp"
#include "Game/Camera/CameraFixedThere.hpp"
#include "Game/Camera/CameraFollow.hpp"
#include "Game/Camera/CameraFooFighter.hpp"
#include "Game/Camera/CameraFooFighterPlanet.hpp"
#include "Game/Camera/CameraFrontAndBack.hpp"
#include "Game/Camera/CameraGround.hpp"
#include "Game/Camera/CameraInnerCylinder.hpp"
#include "Game/Camera/CameraInwardSphere.hpp"
#include "Game/Camera/CameraInwardTower.hpp"
#include "Game/Camera/CameraMedianPlanet.hpp"
#include "Game/Camera/CameraMedianTower.hpp"
#include "Game/Camera/CameraMtxRegParallel.hpp"
#include "Game/Camera/CameraObjParallel.hpp"
#include "Game/Camera/CameraParallel.hpp"
#include "Game/Camera/CameraRaceFollow.hpp"
#include "Game/Camera/CameraRailDemo.hpp"
#include "Game/Camera/CameraRailFollow.hpp"
#include "Game/Camera/CameraRailWatch.hpp"
#include "Game/Camera/CameraSlide.hpp"
#include "Game/Camera/CameraSpiral.hpp"
#include "Game/Camera/CameraSubjective.hpp"
#include "Game/Camera/CameraTalk.hpp"
#include "Game/Camera/CameraTower.hpp"
#include "Game/Camera/CameraTowerPos.hpp"
#include "Game/Camera/CameraTripodBoss.hpp"
#include "Game/Camera/CameraTripodBossJoint.hpp"
#include "Game/Camera/CameraTripodPlanet.hpp"
#include "Game/Camera/CameraTrundle.hpp"
#include "Game/Camera/CameraTwistedPassage.hpp"
#include "Game/Camera/CameraWaterFollow.hpp"
#include "Game/Camera/CameraWaterPlanet.hpp"
#include "Game/Camera/CameraWaterPlanetBoss.hpp"
#include "Game/Camera/CameraWonderPlanet.hpp"
#include <cstring>

struct CameraTableEntry {
    /* 0x00 */ const char* mName;
    /* 0x04 */ const char* mExplain;
    /* 0x08 */ Camera* (*mCreateFunc)(void);
    /* 0x0C */ bool mIsPublic;
};

namespace {
    template < typename T >
    Camera* createCamera() {
        return new T();
    }

    static const CameraTableEntry sCameraTable[] = {
        {"CAM_TYPE_XZ_PARA", "\x95\xc0\x8d\x73", createCamera< CameraParallel >, true},
        {"CAM_TYPE_TOWER", "\x93\x83", createCamera< CameraTower >, true},
        {"CAM_TYPE_FOLLOW", "\x83\x74\x83\x48\x83\x8d\x81\x5b", createCamera< CameraFollow >, true},
        {"CAM_TYPE_WONDER_PLANET", "\x83\x76\x83\x89\x83\x6c\x83\x62\x83\x67", createCamera< CameraWonderPlanet >, true},
        {"CAM_TYPE_POINT_FIX", "\x8a\xae\x91\x53\x8c\xc5\x92\xe8", createCamera< CameraFix >, true},
        {"CAM_TYPE_EYEPOS_FIX", "\x92\xe8\x93\x5f", createCamera< CameraFixedPoint >, true},
        {"CAM_TYPE_SLIDER", "\x83\x58\x83\x89\x83\x43\x83\x5f\x81\x5b", createCamera< CameraBehind >, true},
        {"CAM_TYPE_INWARD_TOWER", "\x93\x83\x93\xe0\x95\x94", createCamera< CameraInwardTower >, true},
        {"CAM_TYPE_EYEPOS_FIX_THERE", "\x82\xbb\x82\xcc\x8f\xea\x92\xe8\x93\x5f", createCamera< CameraFixedThere >, true},
        {"CAM_TYPE_TRIPOD_BOSS", "\x8e\x4f\x8b\x72\x83\x7b\x83\x58", createCamera< CameraTripodBoss >, true},
        {"CAM_TYPE_TOWER_POS", "\x93\x83\x81\x69\x83\x54\x83\x75\x83\x5e\x81\x5b\x83\x51\x83\x62\x83\x67\x95\x74\x82\xab\x81\x6a", createCamera< CameraTowerPos >, true},
        {"CAM_TYPE_TRIPOD_PLANET", "\x8e\x4f\x8b\x72\x98\x66\x90\xaf", createCamera< CameraTripodPlanet >, true},
        {"CAM_TYPE_DEAD", "\x92\xca\x8f\xed\x8e\x80\x96\x53", createCamera< CameraDead >, true},
        {"CAM_TYPE_INWARD_SPHERE", "\x8b\x85\x93\xe0\x95\x94", createCamera< CameraInwardSphere >, true},
        {"CAM_TYPE_RAIL_DEMO", "\x83\x8c\x81\x5b\x83\x8b\x83\x66\x83\x82", createCamera< CameraRailDemo >, true},
        {"CAM_TYPE_RAIL_FOLLOW", "\x83\x8c\x81\x5b\x83\x8b\x83\x74\x83\x48\x83\x8d\x81\x5b", createCamera< CameraRailFollow >, true},
        {"CAM_TYPE_TRIPOD_BOSS_JOINT", "\x8e\x4f\x8b\x72\x83\x7b\x83\x58\x83\x57\x83\x87\x83\x43\x83\x93\x83\x67", createCamera< CameraTripodBossJoint >, true},
        {"CAM_TYPE_CHARMED_TRIPOD_BOSS", "\x8e\x4f\x8b\x72\x83\x7b\x83\x58\x83\x57\x83\x87\x83\x43\x83\x93\x83\x67\x92\x8d\x8e\x8b", createCamera< CameraCharmedTripodBoss >, true},
        {"CAM_TYPE_OBJ_PARALLEL", "\x83\x49\x83\x75\x83\x57\x83\x46\x95\xc0\x8d\x73", createCamera< CameraObjParallel >, true},
        {"CAM_TYPE_CHARMED_FIX", "\x83\x54\x83\x93\x83\x7b", createCamera< CameraCharmedFix >, true},
        {"CAM_TYPE_GROUND", "\x92\x6e\x96\xca", createCamera< CameraGround >, true},
        {"CAM_TYPE_TRUNDLE", "\x83\x67\x83\x89\x83\x93\x83\x68\x83\x8b", createCamera< CameraTrundle >, true},
        {"CAM_TYPE_CUBE_PLANET", "\x83\x4c\x83\x85\x81\x5b\x83\x75\x98\x66\x90\xaf", createCamera< CameraCubePlanet >, true},
        {"CAM_TYPE_INNER_CYLINDER", "\x89\x7e\x93\x9b\x93\xe0\x95\x94", createCamera< CameraInnerCylinder >, true},
        {"CAM_TYPE_SPIRAL_DEMO", "\x97\x86\x90\xf9\x83\x66\x83\x82", createCamera< CameraSpiral >, true},
        {"CAM_TYPE_TALK", "\x89\xef\x98\x62", createCamera< CameraTalk >, true},
        {"CAM_TYPE_MTXREG_PARALLEL", "\x83\x7d\x83\x67\x83\x8a\x83\x4e\x83\x58\x83\x8c\x83\x57\x83\x58\x83\x5e\x95\xc0\x8d\x73", createCamera< CameraMtxRegParallel >, true},
        {"CAM_TYPE_CHARMED_VECREG", "\x83\x78\x83\x4e\x83\x67\x83\x8b\x83\x8c\x83\x57\x83\x58\x83\x5e\x92\x8d\x96\xda", createCamera< CameraCharmedVecReg >, true},
        {"CAM_TYPE_MEDIAN_PLANET", "\x92\x86\x93\x5f\x92\x8d\x96\xda\x83\x76\x83\x89\x83\x6c\x83\x62\x83\x67", createCamera< CameraMedianPlanet >, true},
        {"CAM_TYPE_TWISTED_PASSAGE", "\x82\xcb\x82\xb6\x82\xea\x89\xf1\x98\x4c", createCamera< CameraTwistedPassage >, true},
        {"CAM_TYPE_MEDIAN_TOWER", "\x92\x86\x93\x5f\x93\x83\x83\x4a\x83\x81\x83\x89", createCamera< CameraMedianTower >, true},
        {"CAM_TYPE_CHARMED_VECREG_TOWER", "VecReg\x8a\x70\x93\x78\x95\xe2\x90\xb3\x93\x83\x83\x4a\x83\x81\x83\x89", createCamera< CameraCharmedVecRegTower >, true},
        {"CAM_TYPE_FRONT_AND_BACK", "\x95\x5c\x97\xa0\x83\x4a\x83\x81\x83\x89", createCamera< CameraFrontAndBack >, true},
        {"CAM_TYPE_RACE_FOLLOW", "\x83\x8c\x81\x5b\x83\x58\x97\x70\x83\x74\x83\x48\x83\x8d\x81\x5b", createCamera< CameraRaceFollow >, true},
        {"CAM_TYPE_2D_SLIDE", "\x82\x51\x82\x63\x83\x58\x83\x89\x83\x43\x83\x68", createCamera< CameraSlide >, true},
        {"CAM_TYPE_FOO_FIGHTER", "\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b", createCamera< CameraFooFighter >, true},
        {"CAM_TYPE_FOO_FIGHTER_PLANET", "\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x83\x76\x83\x89\x83\x6c\x83\x62\x83\x67", createCamera< CameraFooFighterPlanet >, true},
        {"CAM_TYPE_BLACK_HOLE", "\x83\x75\x83\x89\x83\x62\x83\x4e\x83\x7a\x81\x5b\x83\x8b", createCamera< CameraBlackHole >, false},
        {"CAM_TYPE_ANIM", "\x83\x41\x83\x6a\x83\x81", createCamera< CameraAnim >, false},
        {"CAM_TYPE_DPD", "\x82\x63\x82\x6f\x82\x63", createCamera< CameraDPD >, true},
        {"CAM_TYPE_WATER_FOLLOW", "\x90\x85\x92\x86\x83\x74\x83\x48\x83\x8d\x81\x5b", createCamera< CameraWaterFollow >, true},
        {"CAM_TYPE_WATER_PLANET", "\x90\x85\x92\x86\x83\x76\x83\x89\x83\x6c\x83\x62\x83\x67", createCamera< CameraWaterPlanet >, true},
        {"CAM_TYPE_WATER_PLANET_BOSS", "\x90\x85\x92\x86\x83\x76\x83\x89\x83\x6c\x83\x62\x83\x67\x83\x7b\x83\x58", createCamera< CameraWaterPlanetBoss >, true},
        {"CAM_TYPE_RAIL_WATCH", "\x83\x8c\x81\x5b\x83\x8b\x92\x8d\x96\xda", createCamera< CameraRailWatch >, true},
        {"CAM_TYPE_SUBJECTIVE", "\x8e\xe5\x8a\xcf", createCamera< CameraSubjective >, true},
    };
    static const char* sDefaultCamera = "CAM_TYPE_XZ_PARA";
};  // namespace

CameraHolder::CameraHolder(const char* pName) : NameObj(pName) {
    createCameras();

    mDefaultCameraIndex = getIndexOf(getNameStrOfDefault());
    mDefaultCamera = getDefaultCamera();
}

s32 CameraHolder::getNum() const {
    return ARRAY_SIZE(::sCameraTable);
}

CamTranslatorBase* CameraHolder::getTranslator(s32 index) {
    return mTranslators[index];
}

s32 CameraHolder::getIndexOf(const char* pName) const {
    for (s32 i = 0; i < getNum(); i++) {
        if (strcmp(pName, getNameStrOf(i)) == 0) {
            return i;
        }
    }

    return -1;
}

const char* CameraHolder::getNameStrOf(s32 index) const {
    return ::sCameraTable[index].mName;
}

const char* CameraHolder::getExplainStrOf(s32 index) const {
    return ::sCameraTable[index].mExplain;
}

bool CameraHolder::isPublic(s32 index) const {
    return ::sCameraTable[index].mIsPublic;
}

Camera* CameraHolder::getDefaultCamera() {
    return mCameras[mDefaultCameraIndex];
}

s32 CameraHolder::getIndexOfDefault() const {
    return mDefaultCameraIndex;
}

const char* CameraHolder::getNameStrOfDefault() const {
    return ::sDefaultCamera;
}

s32 CameraHolder::getIndexOf(Camera* pCamera) const {
    for (s32 i = 0; i < getNum(); i++) {
        if (mCameras[i] == pCamera) {
            return i;
        }
    }

    return -1;
}

void CameraHolder::createCameras() {
    mCameras = new Camera*[getNum()];
    mTranslators = new CamTranslatorBase*[getNum()];

    for (s32 i = 0; i < getNum(); i++) {
        mCameras[i] = ::sCameraTable[i].mCreateFunc();
        mTranslators[i] = mCameras[i]->createTranslator();
    }
}

Camera* CameraHolder::getCameraInner(s32 index) const {
    return mCameras[index];
}
