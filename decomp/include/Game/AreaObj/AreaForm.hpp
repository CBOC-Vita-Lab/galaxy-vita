#pragma once

#include <JSystem/JGeometry/TMatrix.hpp>
#include <JSystem/JGeometry.hpp>

class JMapInfoIter;

class AreaForm {
public:
    enum Type {
        Type_Cube1,
        Type_Cube2,
        Type_Sphere,
        Type_Cylinder,
        Type_Bowl,
    };

    AreaForm() : _4() {
    }

    virtual void init(const JMapInfoIter&);
    virtual bool isInVolume(const TVec3f&) const;

    TPos3f* _4;
};

class AreaFormCube : public AreaForm {
public:
    AreaFormCube(int);

    virtual void init(const JMapInfoIter&);
    virtual bool isInVolume(const TVec3f&) const;

    void calcWorldPos(TVec3f*) const;
    void calcWorldRotate(TVec3f*) const;
    void calcWorldBox(TDirBox3f*) const;
    void calcLocalPos(TVec3f*, const TVec3f&) const;
    static f32 getBaseSize();

    f32 getSizeX() const {
        return mScale.x * getBaseSize();
    }

    f32 getSizeY() const {
        return mScale.y * getBaseSize();
    }

    f32 getSizeZ() const {
        return mScale.z * getBaseSize();
    }

    void updateBoxParam();
    void calcWorldMtx(TPos3f*) const;
#ifdef TARGET_PC
    void portUpdateReject();
    void portSnapshot(f32*) const;
#endif

    /* 0x08 */ int _8;
    /* 0x0C */ TVec3f mTranslation;
    /* 0x18 */ TVec3f mRotation;
    /* 0x24 */ TVec3f mScale;
    /* 0x30 */ TBox3f mBounding;
    /* 0x48 */ TPos3f _48;
#ifdef TARGET_PC
    // GXV_HL_AREACUBE (hivelogic, AreaForm.cpp): a world box around the cube with a margin, for a quick "outside" answer while the
    // cube follows nothing (_4 null). Set by updateBoxParam, the only writer of _48 and mBounding.
    /* 0x78 */ f32 mPortRejectLo[3];
    /* 0x84 */ f32 mPortRejectHi[3];
    /* 0x90 */ u8 mPortRejectMode;   // 0 no box (the full test always), 1 box used, 2 verify (both computed, the full answer used)
    /* 0x94 */ f32 mPortSnap[18];    // _48 and mBounding as the box was built from them (mode 2 checks they have not changed)
    /* 0xDC */ const char* mPortOwner;  // the area's name, for the verify log (AreaObj.cpp)
#endif
};

class AreaFormSphere : public AreaForm {
public:
    AreaFormSphere();

    virtual void init(const JMapInfoIter&);
    virtual bool isInVolume(const TVec3f&) const;

    void calcUpVec(TVec3f*) const;
    void calcPos(TVec3f*) const;

    /* 0x08 */ TVec3f mTranslation;
    /* 0x14 */ f32 mRadius;
    /* 0x18 */ TVec3f mUp;
};

class AreaFormBowl : public AreaForm {
public:
    AreaFormBowl();

    virtual void init(const JMapInfoIter&);
    virtual bool isInVolume(const TVec3f&) const;

    void calcUpVec(const TVec3f&);

    /* 0x08 */ TVec3f mTranslation;
    /* 0x14 */ TVec3f mUp;
    /* 0x20 */ f32 _20;
};

class AreaFormCylinder : public AreaForm {
public:
    AreaFormCylinder();

    virtual void init(const JMapInfoIter&);
    virtual bool isInVolume(const TVec3f&) const;

    void calcPos(TVec3f*) const;
    void calcCenterPos(TVec3f*) const;
    void calcUpVec(TVec3f*) const;
    void calcDir(const TVec3f&);

    /* 0x08 */ TVec3f mTranslation;
    /* 0x14 */ TVec3f mRotation;
    /* 0x20 */ f32 mRadius;
    /* 0x24 */ f32 mHeight;
};
