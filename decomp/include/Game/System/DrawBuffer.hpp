#pragma once

#include "Game/LiveActor/LiveActor.hpp"

class J3DMaterial;
class J3DMatPacket;
class J3DModelData;
class J3DModel;
class J3DShapePacket;
class J3DDisplayListObj;
// class LiveActor;
class ActorLightCtrl;

struct PacketInfo {
    /* 0x00 */ J3DShapePacket* mShapePacket;
    /* 0x04 */ ActorLightCtrl* mLightLoader;
    /* 0x08 */ ActorLightCtrl* mLightCtrl;
};

class DrawBufferShapeDrawer {
public:
    DrawBufferShapeDrawer(J3DMaterial*, J3DMatPacket*);

    void init(s32);
    void swap(DrawBufferShapeDrawer*);
    void draw() const;
    void add(const J3DShapePacket*, const ActorLightCtrl*);
    void remove(const J3DShapePacket*);
    void resetLightSort(const ActorLightCtrl*);
    s32 findLightSortIndex(const ActorLightCtrl*, bool*) const;

    J3DShapePacket* getShapePacket(int index) const {
        return mPackets[index]->mShapePacket;
    }

    ActorLightCtrl* getLightLoader(int index) const {
        return mPackets[index]->mLightLoader;
    }

    /* 0x00 */ J3DMaterial* mMaterial;
    /* 0x04 */ J3DMatPacket* mMatPacket;
    /* 0x08 */ s32 _8;
    /* 0x0C */ s32 mMaxPackets;
    /* 0x10 */ s32 mNumPackets;
    /* 0x14 */ PacketInfo** mPackets;
#ifdef TARGET_PC
    // (1.3 port, GXV_J3D_MATMERGE / GXV_J3D_DIFFSKIP: DrawBuffer.cpp)
    void drawPort() const;
    bool callMergedList(J3DDisplayListObj* pMatDL, J3DDisplayListObj* pDiffDL) const;
    mutable u8* mMergedDL;        // the material list and the first packet's differed list as one (J3DPortBuildMerged; guest memory: the
                                  // drawer's heap), followed by a copy of the two lists it was made of
    mutable u32 mMergedCap;       // the room of the block
    mutable u32 mMergedSize;      // the merged list's size
    mutable u32 mMergedMatSize;   // the sizes of the two lists it was made of (0: none)
    mutable u32 mMergedDiffSize;
    mutable u32 mMergedLastCall;  // the picture that last called it (J3DPortPicture), 0: never
    mutable u32 mMergedQuietUntil;
    mutable u32 mMergedChanges;   // made again this many times in a row without being called
#endif
};

class DrawBuffer {
public:
    DrawBuffer(J3DModel* pModel);

    void init(int);
    void add(const LiveActor*);
    void remove(const LiveActor*);
    void resetLightSort(const ActorLightCtrl*);
    void drawOpa() const;
    void drawXlu() const;
    void initTable();
    void sortShapeDrawer(s32, s32);

    DrawBufferShapeDrawer* getShapeDrawerByMatNo(int matNo) const {
        return mShapeDrawers[mMaterialNos[matNo]];
    }

    DrawBufferShapeDrawer* getShapeDrawerByIndex(int index) const {
        return mShapeDrawers[index];
    }

    /* 0x00 */ J3DModelData* mModelData;
    /* 0x04 */ J3DModel* mModel;
    /* 0x08 */ u32 _8;
    /* 0x0C */ s32 mNumActors;
    /* 0x10 */ u32 _10;
    /* 0x14 */ s32 mNumMaterials;
    /* 0x18 */ s32* mMaterialNos;
    /* 0x1C */ s32 mNumShapeDrawers;
    /* 0x20 */ s32 mNumOpaShapeDrawers;
    /* 0x24 */ DrawBufferShapeDrawer** mShapeDrawers;
};
