#pragma once

#include <revolution.h>

enum J3DSysDrawBuf {
    /* 0x0 */ J3DSysDrawBuf_Opa,
    /* 0x1 */ J3DSysDrawBuf_Xlu,

    /* 0x2 */ J3DSysDrawBuf_MAX
};

struct J3DTexCoordScaleInfo {
    /* 0x0 */ u16 field_0x00;
    /* 0x2 */ u16 field_0x02;
    /* 0x4 */ u16 field_0x04;
    /* 0x6 */ u16 field_0x06;
};

enum J3DError {
    kJ3DError_Success = 0,
    kJ3DError_Alloc = 4,
};

#ifdef TARGET_PC
// GXV_PAR_VIEW=2 (decomp/src/Game/LiveActor/ModelManager.cpp): while the drawn turn's view jobs are outstanding (finished on a worker
// during the draw), a draw or normal half is made complete before it is handed to GX below.
extern bool gPortParViewAsync;
extern "C" void port_par_view_touch_out(const void* out);
#endif

class J3DMtxCalc;
class J3DModel;
class J3DMatPacket;
class J3DShapePacket;
class J3DShape;
class J3DDrawBuffer;
class J3DTexture;

enum J3DSysFlag {
    J3DSysFlag_SkinPosCpu = 0x00000004,
    J3DSysFlag_SkinNrmCpu = 0x00000008,
    J3DSysFlag_PostTexMtx = 0x40000000,
};

class J3DSys {
public:
    /* 0x000 */ Mtx mViewMtx;
    /* 0x030 */ J3DMtxCalc* mCurrentMtxCalc;
    /* 0x034 */ u32 mFlags;
    /* 0x038 */ J3DModel* mModel;
    /* 0x03C */ J3DMatPacket* mMatPacket;
    /* 0x040 */ J3DShapePacket* mShapePacket;
    /* 0x044 */ J3DShape* mShape;
    /* 0x048 */ J3DDrawBuffer* mDrawBuffer[J3DSysDrawBuf_MAX];
    /* 0x050 */ u32 mDrawMode;
    /* 0x054 */ u32 mMaterialMode;
    /* 0x058 */ J3DTexture* mTexture;
    /* 0x05C */ u8 unk_0x5c[0x60 - 0x5C];
    /* 0x060 */ u32 mTexCacheRegionNum;
    /* 0x064 */ GXTexRegion mTexCacheRegion[8];
    /* 0x0E4 */ u8 unk_0xe4[0x104 - 0xE4];
    /* 0x104 */ Mtx* mModelDrawMtx;
    /* 0x108 */ Mtx33* mModelNrmMtx;
    /* 0x10C */ void* mVtxPos;
    /* 0x110 */ void* mVtxNrm;
    /* 0x114 */ GXColor* mVtxCol;
    /* 0x118 */ Vec* mNBTScale;

    J3DSys();

    void loadPosMtxIndx(int, u16) const;
    void loadNrmMtxIndx(int, u16) const;
    void setTexCacheRegion(GXTexCacheSize);
    void drawInit();
    void reinitGX();
    void reinitGenMode();
    void reinitLighting();
    void reinitTransform();
    void reinitTexture();
    void reinitTevStages();
    void reinitIndStages();
    void reinitPixelProc();

    MtxPtr getViewMtx() {
        return mViewMtx;
    }

    void onFlag(u32 flag) {
        mFlags |= flag;
    }
    void offFlag(u32 flag) {
        mFlags &= ~flag;
    }
    bool checkFlag(u32 flag) {
        return mFlags & flag ? true : false;
    }

    J3DMatPacket* getMatPacket() {
        return mMatPacket;
    }
    void setMaterialMode(u32 mode) {
        mMaterialMode = mode;
    }

    void* getVtxPos() {
        return mVtxPos;
    }
    void setVtxPos(void* pVtxPos) {
        mVtxPos = pVtxPos;
    }

    void* getVtxNrm() {
        return mVtxNrm;
    }
    void setVtxNrm(void* pVtxNrm) {
        mVtxNrm = pVtxNrm;
    }

    void* getVtxCol() {
        return mVtxCol;
    }
    void setVtxCol(GXColor* pVtxCol) {
        mVtxCol = pVtxCol;
    }

    void setModelDrawMtx(Mtx* pMtxArr) {
#ifdef TARGET_PC
        if (gPortParViewAsync) {
            port_par_view_touch_out(pMtxArr);  // (GXV_PAR_VIEW=2: the front end's indexed loads and J3DShapeMtx's immediate ones read it)
        }
#endif
        mModelDrawMtx = pMtxArr;
        GXSetArray(GX_POS_MTX_ARRAY, mModelDrawMtx, sizeof(*mModelDrawMtx));
    }

    void setModelNrmMtx(Mtx33* pMtxArr) {
#ifdef TARGET_PC
        if (gPortParViewAsync) {
            port_par_view_touch_out(pMtxArr);
        }
#endif
        mModelNrmMtx = pMtxArr;
        GXSetArray(GX_NRM_MTX_ARRAY, mModelNrmMtx, sizeof(*mModelNrmMtx));
    }

    void setNBTScale(Vec* scale) {
        mNBTScale = scale;
    }
    Vec* getNBTScale() {
        return mNBTScale;
    }

    void setMatPacket(J3DMatPacket* pPacket) {
        mMatPacket = pPacket;
    }

    void setShapePacket(J3DShapePacket* pPacket) {
        mShapePacket = pPacket;
    }

    J3DModel* getModel() {
        return mModel;
    }
    void setModel(J3DModel* pModel) {
        mModel = pModel;
    }

    J3DShapePacket* getShapePacket() {
        return mShapePacket;
    }
    Mtx& getModelDrawMtx(u16 no) {
        return mModelDrawMtx[no];
    }
    void setTexture(J3DTexture* pTex) {
        mTexture = pTex;
    }
    J3DTexture* getTexture() {
        return mTexture;
    }

    J3DDrawBuffer* getDrawBuffer(int type) {
        return mDrawBuffer[type];
    }

    static Mtx mCurrentMtx;
    static Vec mCurrentS;
    static Vec mParentS;
    static J3DTexCoordScaleInfo sTexCoordScaleTable[8];
};

extern u32 j3dDefaultViewNo;
extern J3DSys j3dSys;

#ifdef TARGET_PC
// GXV_PAR_DL (gamedraw23; decomp/src/Game/LiveActor/ModelManager.cpp): the drawn turn's material lists may be made by a worker while the
// game thread makes the view pass. The material path (J3DModel::calcMaterial, J3DMaterial::calc / diff / makeDisplayList / patch, the
// texture-generator calc, the GD loads of the differed lists, DisplayListMaker::diff) reads and writes the J3DSys J3D_MSYS names: j3dSys
// itself, except during that overlap, when it is the worker's private copy (made from j3dSys at the dispatch; merged back at the join).
// gPortParDlRun: the overlap is open; the material path is the worker's (the guest-OS calls it makes on the game thread, interrupts and
// the scheduler, are made once for the whole overlap by the game thread instead). PortDlGameEnter() at each way into the material path:
// on the game thread during the overlap it first finishes the overlap (port_par_dl_game_enter), so the material path never runs on two
// threads at once. All three are defined in J3DPacket.cpp (the function is weak there: J3D alone, in a host test, has no overlap).
// gPortParDlStackLo / _Hi: the worker's stack (set by the game thread before each dispatch): a frame inside it is the worker's, which goes
// on without a call (anything else asks port_par_dl_game_enter, which also knows the game thread running a job it took).
extern J3DSys* gPortJ3DSysMat;
extern bool gPortParDlRun;
extern unsigned long gPortParDlStackLo, gPortParDlStackHi;
extern "C" void port_par_dl_game_enter(void);
#define J3D_MSYS (*gPortJ3DSysMat)
inline void PortDlGameEnter() {
    if (__builtin_expect(gPortParDlRun, 0)) {
        const unsigned long sp = reinterpret_cast< unsigned long >(__builtin_frame_address(0));
        if (sp - gPortParDlStackLo < gPortParDlStackHi - gPortParDlStackLo) {
            return;  // (the worker)
        }
        port_par_dl_game_enter();
    }
}
// GXV_DRAWPIPE (dpengine, 11 Nov; port/draw_pipe.h, decomp/src/Game/System/DrawBufferHolder.cpp): the draw path's J3DSys. The J3D draw
// passes (DrawBufferGroup / DrawBufferShapeDrawer -> J3DShapePacket::prepareDraw -> J3DShape::drawFast -> J3DShapeMtx, the user lists'
// texture-matrix loads) read and write the J3DSys J3D_DSYS names: j3dSys itself, except while a pass is drawn from a frame frozen at
// the hand-over. Then it is the draw side's own (DrawBufferHolder.cpp: j3dSys as it stood at the hand-over, plus every word the game
// thread's draw lists changed up to the pass, plus what the passes before it wrote), so that a pass drawn later, on another thread
// beside the next update, reads the J3DSys the serial draw would have shown it; j3dSys gets the passes' words back when the draw is
// joined. A shape packet's base matrix may point INTO j3dSys (the view matrix, J3DModel::prepareShapePackets): PortBaseMtx follows the
// same indirection.
extern J3DSys* gPortJ3DSysDraw;
extern const J3DShapePacket* gPortPipePrepared;  // (J3DPacket.cpp)
#define J3D_DSYS (*gPortJ3DSysDraw)
inline Mtx* PortBaseMtx(Mtx* pBase) {
    return pBase == reinterpret_cast< Mtx* >(j3dSys.mViewMtx) ? reinterpret_cast< Mtx* >(gPortJ3DSysDraw->mViewMtx) : pBase;
}
#else
#define J3D_MSYS j3dSys
#define J3D_DSYS j3dSys
inline void PortDlGameEnter() {}
inline Mtx* PortBaseMtx(Mtx* pBase) { return pBase; }
#endif
