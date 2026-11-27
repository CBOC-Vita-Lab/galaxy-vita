#pragma once

#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <revolution/types.h>

class BpkPlayer;
class BrkPlayer;
class BtkPlayer;
class BtpPlayer;
class BvaPlayer;
class DisplayListMaker;
class J3DFrameCtrl;
class J3DModel;
class J3DModelData;
class ResourceHolder;
class XanimePlayer;
class XanimeResourceTable;
class XjointTransform;

class ModelManager {
public:
    /// @brief Creates a new `ModelManager`.
    ModelManager();

    void update();
    void calcAnim();
    void calcView();
    void entry();
    void newDifferedDLBuffer();
    void updateDL(bool);
    void startBck(const char*, const char*);
    void startBckWithInterpole(const char*, s32);
    void startBtk(const char*);
    void startBrk(const char*);
    void startBtp(const char*);
    void startBpk(const char*);
    void startBva(const char*);
    void stopBtk();
    void stopBrk();
    void stopBtp();
    void stopBpk();
    void stopBva();
    J3DFrameCtrl* getBckCtrl() const;
    J3DFrameCtrl* getBtkCtrl() const;
    J3DFrameCtrl* getBrkCtrl() const;
    J3DFrameCtrl* getBtpCtrl() const;
    J3DFrameCtrl* getBpkCtrl() const;
    J3DFrameCtrl* getBvaCtrl() const;
    bool isBckStopped() const;
    bool isBtkStopped() const;
    bool isBrkStopped() const;
    bool isBtpStopped() const;
    bool isBpkStopped() const;
    bool isBvaStopped() const;
    bool isBtkPlaying(const char*) const;
    bool isBrkPlaying(const char*) const;
    bool isBpkPlaying(const char*) const;
    bool isBtpPlaying(const char*) const;
    bool isBvaPlaying(const char*) const;
    void initJointTransform();
    XjointTransform* getJointTransform(const char*);
    ResourceHolder* getResourceHolder() const;
    ResourceHolder* getModelResourceHolder() const;
    J3DModel* getJ3DModel() const;
    J3DModelData* getJ3DModelData() const;
    const char* getPlayingBckName() const;
    void initModelAndAnimation(ResourceHolder*, const char*, ResourceHolder*, J3DMdlFlag);
    void initMaterialAnm();
    void initVisibilityAnm();
    void calc();
    void changeBckSetting(const char*, const char*, XanimePlayer*);
    void init(const char*, const char*, bool);

    /* 0x00 */ BtkPlayer* mBtkPlayer;
    /* 0x04 */ BrkPlayer* mBrkPlayer;
    /* 0x08 */ BtpPlayer* mBtpPlayer;
    /* 0x0C */ BpkPlayer* mBpkPlayer;
    /* 0x10 */ BvaPlayer* mBvaPlayer;
    /* 0x14 */ XanimeResourceTable* mXanimeResourceTable;
    /* 0x18 */ XanimePlayer* mXanimePlayer;
    /* 0x1C */ J3DModel* mModel;
    /* 0x20 */ ResourceHolder* mModelResourceHolder;
    /* 0x24 */ DisplayListMaker* mDisplayListMaker;
#ifdef TARGET_PC
    bool mStaleDL = false;  // (GXV_DL_LAZY) materials not computed since a turn without a picture; listed for the next drawn turn
    void* mAnimMemo = nullptr;  // (GXV_ANIM_MEMO) the inputs and the result of the last joint computation (ModelManager.cpp)
    u32 mAnimMemoGen = 0;       // (GXV_ANIM_MEMO) the memo store's generation mAnimMemo belongs to (a scene change starts a new one)
    void* mViewMemo = nullptr;  // (GXV_VIEW_KEEP) the inputs of the last view pass and where it wrote (ModelManager.cpp)
    u32 mViewMemoGen = 0;       // (GXV_VIEW_KEEP) the memo store's generation mViewMemo belongs to
    void* mParAnim = nullptr;   // (GXV_PAR_ANIM) this model's speculation record: its job in the open batch, its misses (ModelManager.cpp)
    u32 mParAnimGen = 0;        // (GXV_PAR_ANIM) the record store's generation mParAnim belongs to
    u64 mDlTmHash = 0;          // (GXV_DL_STATIC_TM) the content of the plain texture matrices after the last full computation (ModelManager.cpp DlStaticTmHash)
    u8 mDlStatic = 0;           // (GXV_DL_STATIC) 0: not known; 1: its materials were computed in full, with the post-texture-matrix flag clear, and cannot change; 2: the same with the flag set
    // (GXV_DL_LAZY_KEY, dlcrash 8 Nov) the models the deferred list refresh was last asked for: getJ3DModel() and the display-list
    // maker's model at the time. MarioActor::changeDisplayMode points both at another body (bee, ice, ...) between turns; the flush
    // computes only what was asked for (ModelManager.cpp port_dl_flush_stale).
    J3DModel* mStaleDLModel = nullptr;
    J3DModel* mStaleDLDiffModel = nullptr;
    // (GXV_MATLOD, cpulod26) the material list was refreshed at least once; turns left out since the last refresh (0xFF fresh / not far); the model was far
    // enough this turn for the rule to apply (the census times its refreshes)
    u8 mCpDlMade = 0;
    u8 mCpDlSkip = 0xFF;
    u8 mCpDlFar = 0;
    // (GXV_MATCALM, cpu34d) the flush's refreshes in a row that stored no changed differed list (saturates at 255; 0 at any start of a
    // material animation); flushes left out since the last refresh
    u8 mCalmRun = 0;
    u8 mCalmSkip = 0;
#endif
};
