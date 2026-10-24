#pragma once

#include <revolution.h>

class J3DModelData;
class J3DMaterialTable;
class J3DModelHierarchy;

enum J3DModelLoaderFlagTypes {
    J3DMLF_None = 0x00000000,

    J3DMLF_MtxSoftImageCalc = 0x00000001,
    J3DMLF_MtxMayaCalc = 0x00000002,
    J3DMLF_MtxBasicCalc = 0x00000004,
    J3DMLF_04 = 0x00000008,
    J3DMLF_MtxTypeMask =
        J3DMLF_MtxSoftImageCalc | J3DMLF_MtxMayaCalc | J3DMLF_MtxBasicCalc | J3DMLF_04,  // 0 - 2 (0 = Basic, 1 = SoftImage, 2 = Maya)

    J3DMLF_UseImmediateMtx = 0x00000010,
    J3DMLF_UsePostTexMtx = 0x00000020,
    J3DMLF_07 = 0x00000040,
    J3DMLF_08 = 0x00000080,
    J3DMLF_NoMatrixTransform = 0x00000100,
    J3DMLF_10 = 0x00000200,
    J3DMLF_11 = 0x00000400,
    J3DMLF_12 = 0x00000800,
    J3DMLF_13 = 0x00001000,
    J3DMLF_DoBdlMaterialCalc = 0x00002000,
    J3DMLF_15 = 0x00004000,
    J3DMLF_16 = 0x00008000,
    J3DMLF_TevNumShift = 0x00010000,
    J3DMLF_18 = 0x00020000,
    J3DMLF_UseSingleSharedDL = 0x00040000,
    J3DMLF_20 = 0x00080000,
    J3DMLF_21 = 0x00100000,
    J3DMLF_UseUniqueMaterials = 0x00200000,
    J3DMLF_23 = 0x00400000,
    J3DMLF_24 = 0x00800000,
    J3DMLF_Material_UseIndirect = 0x01000000,
    J3DMLF_26 = 0x02000000,
    J3DMLF_27 = 0x04000000,
    J3DMLF_Material_TexGen_Block4 = 0x08000000,
    J3DMLF_Material_PE_Full = 0x10000000,
    J3DMLF_Material_PE_FogOff = 0x20000000,
    J3DMLF_Material_Color_LightOn = 0x40000000,
    J3DMLF_Material_Color_AmbientOn = 0x80000000
};

struct J3DModelBlock {
    u32 mBlockType;
    u32 mBlockSize;
};

struct J3DModelFileData {
    u32 mMagic1;
    u32 mMagic2;
    u8 _8[4];
    u32 mBlockNum;
    u8 _10[0x10];
    J3DModelBlock mBlocks[1];
};

struct J3DModelInfoBlock : public J3DModelBlock {
    u16 mFlags;
    u32 mPacketNum;
    u32 mVtxNum;
    PTR32(void) mpHierarchy;
};

struct J3DVertexBlock : public J3DModelBlock {
    PTR32(void) mpVtxAttrFmtList;
    PTR32(void) mpVtxPosArray;
    PTR32(void) mpVtxNrmArray;
    PTR32(void) mpVtxNBTArray;
    PTR32(void) mpVtxColorArray[2];
    PTR32(void) mpVtxTexCoordArray[8];
};

struct J3DEnvelopeBlock : public J3DModelBlock {
    u16 mWEvlpMtxNum;
    PTR32(void) mpWEvlpMixMtxNum;
    PTR32(void) mpWEvlpMixIndex;
    PTR32(void) mpWEvlpMixWeight;
    PTR32(void) mpInvJointMtx;
};

struct J3DDrawBlock : public J3DModelBlock {
    u16 mMtxNum;
    PTR32(void) mpDrawMtxFlag;
    PTR32(void) mpDrawMtxIndex;
};

struct J3DJointBlock : public J3DModelBlock {
    /* 0x08 */ u16 mJointNum;
    /* 0x0C */ PTR32(void) mpJointInitData;
    /* 0x10 */ PTR32(void) mpIndexTable;
    /* 0x14 */ PTR32(void) mpNameTable;
};  // size 0x18

struct J3DMaterialBlock : public J3DModelBlock {
    u16 mMaterialNum;
    PTR32(void) mpMaterialInitData;
    PTR32(void) mpMaterialID;
    PTR32(void) mpNameTable;
    PTR32(void) mpIndInitData;
    PTR32(void) mpCullMode;
    PTR32(void) mpMatColor;
    PTR32(void) mpColorChanNum;
    PTR32(void) mpColorChanInfo;
    PTR32(void) mpAmbColor;
    PTR32(void) mpLightInfo;
    PTR32(void) mpTexGenNum;
    PTR32(void) mpTexCoordInfo;
    PTR32(void) mpTexCoord2Info;
    PTR32(void) mpTexMtxInfo;
    PTR32(void) field_0x44;
    PTR32(void) mpTexNo;
    PTR32(void) mpTevOrderInfo;
    PTR32(void) mpTevColor;
    PTR32(void) mpTevKColor;
    PTR32(void) mpTevStageNum;
    PTR32(void) mpTevStageInfo;
    PTR32(void) mpTevSwapModeInfo;
    PTR32(void) mpTevSwapModeTableInfo;
    PTR32(void) mpFogInfo;
    PTR32(void) mpAlphaCompInfo;
    PTR32(void) mpBlendInfo;
    PTR32(void) mpZModeInfo;
    PTR32(void) mpZCompLoc;
    PTR32(void) mpDither;
    PTR32(void) mpNBTScaleInfo;
};

struct J3DMaterialBlock_v21 : public J3DModelBlock {
    u16 mMaterialNum;
    PTR32(void) mpMaterialInitData;
    PTR32(void) mpMaterialID;
    PTR32(void) mpNameTable;
    PTR32(void) mpCullMode;
    PTR32(void) mpMatColor;
    PTR32(void) mpColorChanNum;
    PTR32(void) mpColorChanInfo;
    PTR32(void) mpTexGenNum;
    PTR32(void) mpTexCoordInfo;
    PTR32(void) mpTexCoord2Info;
    PTR32(void) mpTexMtxInfo;
    PTR32(void) field_0x38;
    PTR32(void) mpTexNo;
    PTR32(void) mpTevOrderInfo;
    PTR32(void) mpTevColor;
    PTR32(void) mpTevKColor;
    PTR32(void) mpTevStageNum;
    PTR32(void) mpTevStageInfo;
    PTR32(void) mpTevSwapModeInfo;
    PTR32(void) mpTevSwapModeTableInfo;
    PTR32(void) mpFogInfo;
    PTR32(void) mpAlphaCompInfo;
    PTR32(void) mpBlendInfo;
    PTR32(void) mpZModeInfo;
    PTR32(void) mpZCompLoc;
    PTR32(void) mpDither;
    PTR32(void) mpNBTScaleInfo;
};

struct J3DMaterialDLBlock : public J3DModelBlock {
    u16 mMaterialNum;
    PTR32(void) mpDisplayListInit;
    PTR32(void) mpPatchingInfo;
    PTR32(void) mpCurrentMtxInfo;
    PTR32(void) mpMaterialMode;
    PTR32(void) _1C;
    PTR32(void) mpNameTable;
};

struct J3DShapeBlock : public J3DModelBlock {
    u16 mShapeNum;          // 0x00
    PTR32(void) mpShapeInitData;  // 0x04
    PTR32(void) mpIndexTable;     // 0x08
    PTR32(void) mpNameTable;      // 0x0C
    PTR32(void) mpVtxDescList;
    PTR32(void) mpMtxTable;
    PTR32(void) mpDisplayListData;
    PTR32(void) mpMtxInitData;
    PTR32(void) mpDrawInitData;
};

struct J3DTextureBlock : public J3DModelBlock {
    u16 mTextureNum;
    PTR32(void) mpTextureRes;
    PTR32(void) mpNameTable;
};

class J3DModelLoader {
public:
    J3DModelLoader();

    virtual J3DModelData* load(const void*, u32);
    virtual J3DMaterialTable* loadMaterialTable(const void*);
    virtual J3DModelData* loadBinaryDisplayList(const void*, u32);
    virtual u32 calcLoadSize(const void*, u32);
    virtual u32 calcLoadMaterialTableSize(const void*);
    virtual u32 calcLoadBinaryDisplayListSize(void const*, u32);
    virtual u16 countMaterialNum(void const*);
    virtual void setupBBoardInfo();
    virtual ~J3DModelLoader() {
    }

    virtual void readMaterial(J3DMaterialBlock const*, u32) {
    }

    virtual void readMaterial_v21(J3DMaterialBlock_v21 const*, u32) {
    }

    virtual void readMaterialTable(J3DMaterialBlock const*, u32) {
    }

    virtual void readMaterialTable_v21(J3DMaterialBlock_v21 const*, u32) {
    }

    virtual u32 calcSizeMaterial(J3DMaterialBlock const*, u32) {
        return false;
    }

    virtual u32 calcSizeMaterialTable(J3DMaterialBlock const*, u32) {
        return false;
    }

    void readInformation(J3DModelInfoBlock const*, u32);
    void readVertex(J3DVertexBlock const*);
    void readEnvelop(J3DEnvelopeBlock const*);
    void readDraw(J3DDrawBlock const*);
    void readJoint(J3DJointBlock const*);
    void readShape(J3DShapeBlock const*, u32);
    void readTexture(J3DTextureBlock const*);
    void readTextureTable(J3DTextureBlock const*);
    void readPatchedMaterial(J3DMaterialBlock const*, u32);
    void readMaterialDL(J3DMaterialDLBlock const*, u32);
    void modifyMaterial(u32);

    u32 calcSizeInformation(J3DModelInfoBlock const*, u32);
    u32 calcSizeJoint(J3DJointBlock const*);
    u32 calcSizeEnvelope(J3DEnvelopeBlock const*);
    u32 calcSizeDraw(J3DDrawBlock const*) NO_INLINE;
    u32 calcSizeShape(J3DShapeBlock const*, u32);
    u32 calcSizeTexture(J3DTextureBlock const*);
    u32 calcSizeTextureTable(J3DTextureBlock const*);
    u32 calcSizePatchedMaterial(J3DMaterialBlock const*, u32);
    u32 calcSizeMaterialDL(J3DMaterialDLBlock const*, u32);

    J3DModelData* mpModelData;                // 0x04
    J3DMaterialTable* mpMaterialTable;        // 0x08
    const J3DShapeBlock* mpShapeBlock;        // 0x0C
    const J3DMaterialBlock* mpMaterialBlock;  // 0x10
    J3DModelHierarchy* mpModelHierarchy;      // 0x14
    u8 field_0x18;
    u8 field_0x19;
    u16 mEnvelopeSize;  // 0x1A
};

class J3DModelLoader_v26 : public J3DModelLoader {
public:
    J3DModelLoader_v26() {
    }

    ~J3DModelLoader_v26() {
    }

    void readMaterial(J3DMaterialBlock const*, u32);
    void readMaterialTable(J3DMaterialBlock const*, u32);
    u32 calcSizeMaterial(J3DMaterialBlock const*, u32);
    u32 calcSizeMaterialTable(J3DMaterialBlock const*, u32);
};

class J3DModelLoader_v21 : public J3DModelLoader {
public:
    inline J3DModelLoader_v21() {
    }

    ~J3DModelLoader_v21() {
    }

    void readMaterial_v21(J3DMaterialBlock_v21 const*, u32);
    void readMaterialTable_v21(J3DMaterialBlock_v21 const*, u32);
};

class J3DModelLoaderDataBase {
public:
    static J3DModelData* load(void const*, u32);
    static J3DMaterialTable* loadMaterialTable(const void*);
    static J3DModelData* loadBinaryDisplayList(const void*, u32);
};

static inline u32 getMdlDataFlag_TevStageNum(u32 flags) {
    return (flags & 0x001f0000) >> 0x10;
}

static inline u32 getMdlDataFlag_TexGenFlag(u32 flags) {
    return flags & 0x0c000000;
}

static inline u32 getMdlDataFlag_ColorFlag(u32 flags) {
    return flags & 0xc0000000;
}

static inline u32 getMdlDataFlag_PEFlag(u32 flags) {
    return flags & 0x30000000;
}

static inline u32 getMdlDataFlag_MtxLoadType(u32 flags) {
    return flags & 0x10;
}
