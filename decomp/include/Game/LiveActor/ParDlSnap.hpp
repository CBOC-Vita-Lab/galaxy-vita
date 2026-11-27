// GXV_PAR_DL=2 (gamedraw23; decomp/src/Game/LiveActor/ModelManager.cpp): the verify's view of the material state the material lists can
// write (per material: the material animations' and material controls' targets, the texture-generator results, the current-matrix words)
// and the fingerprint of one model's outputs (its differed lists' bytes, texture matrices, current-matrix words). Header-only: used by
// ModelManager.cpp and by the host test tests/par_dl.cpp. TARGET_PC only.
#pragma once
#include <JSystem/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3DGraphBase/J3DMatBlock.hpp>
#include <JSystem/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3DGraphBase/J3DTexture.hpp>
#include <cstring>

// (GXV_PAR_DL=2) what the material lists can write in one material
struct ParDlMatSnap {
    J3DMaterial* m;
    J3DGXColorS10 tevColor[4];
    J3DGXColor tevK[4], mat[2], amb[2];
    u16 texNo[8];
    J3DFogInfo fog;
    J3DTexMtx* texMtxPtr[8];
    u8 texMtx[8][sizeof(J3DTexMtx)];
    u8 coord[8][sizeof(J3DTexCoord)];
    u8 cur[sizeof(J3DCurrentMtx)], shapeCur[sizeof(J3DCurrentMtx)];
    J3DMaterialAnm* anm;
};
inline void ParDlSnap(ParDlMatSnap& o, J3DMaterial* m) {
    std::memset(&o, 0, sizeof(o));
    o.m = m;
    for (u32 i = 0; i < 4; ++i) {
        if (const J3DGXColorS10* c = m->getTevBlock()->getTevColor(i)) o.tevColor[i] = *c;
        if (const J3DGXColor* c = m->getTevBlock()->getTevKColor(i)) o.tevK[i] = *c;
    }
    for (u32 i = 0; i < 8; ++i) {
        o.texNo[i] = m->getTevBlock()->getTexNo(i);
        o.texMtxPtr[i] = m->getTexGenBlock()->getTexMtx(i);
        if (o.texMtxPtr[i] != nullptr) std::memcpy(o.texMtx[i], o.texMtxPtr[i], sizeof(J3DTexMtx));
        if (const J3DTexCoord* tc = m->getTexGenBlock()->getTexCoord(i)) std::memcpy(o.coord[i], tc, sizeof(J3DTexCoord));
    }
    for (u32 i = 0; i < 2; ++i) {
        if (const J3DGXColor* c = m->getColorBlock()->getMatColor(i)) o.mat[i] = *c;
        if (const J3DGXColor* c = m->getColorBlock()->getAmbColor(i)) o.amb[i] = *c;
    }
    if (J3DFog* f = m->getPEBlock()->getFog()) o.fog = *static_cast< J3DFogInfo* >(f);
    std::memcpy(o.cur, &m->mCurrentMtx, sizeof(J3DCurrentMtx));
    if (m->getShape() != nullptr) std::memcpy(o.shapeCur, &m->getShape()->mCurrentMtx, sizeof(J3DCurrentMtx));
    o.anm = m->mMaterialAnm;
}
inline void ParDlRestore(const ParDlMatSnap& o) {
    J3DMaterial* m = o.m;
    for (u32 i = 0; i < 4; ++i) {
        if (J3DGXColorS10* c = m->getTevBlock()->getTevColor(i)) *c = o.tevColor[i];
        if (J3DGXColor* c = m->getTevBlock()->getTevKColor(i)) *c = o.tevK[i];
    }
    for (u32 i = 0; i < 8; ++i) {
        m->getTevBlock()->setTexNo(i, o.texNo[i]);
        m->getTexGenBlock()->setTexMtx(i, o.texMtxPtr[i]);
        if (o.texMtxPtr[i] != nullptr) std::memcpy(o.texMtxPtr[i], o.texMtx[i], sizeof(J3DTexMtx));
        if (J3DTexCoord* tc = m->getTexGenBlock()->getTexCoord(i)) std::memcpy(tc, o.coord[i], sizeof(J3DTexCoord));
    }
    for (u32 i = 0; i < 2; ++i) {
        if (J3DGXColor* c = m->getColorBlock()->getMatColor(i)) *c = o.mat[i];
        if (J3DGXColor* c = m->getColorBlock()->getAmbColor(i)) *c = o.amb[i];
    }
    if (J3DFog* f = m->getPEBlock()->getFog()) *static_cast< J3DFogInfo* >(f) = o.fog;
    std::memcpy(&m->mCurrentMtx, o.cur, sizeof(J3DCurrentMtx));
    if (m->getShape() != nullptr) std::memcpy(&m->getShape()->mCurrentMtx, o.shapeCur, sizeof(J3DCurrentMtx));
    m->mMaterialAnm = o.anm;
}
inline bool ParDlSnapSame(const ParDlMatSnap& a, const ParDlMatSnap& b) {
    // (field by field: the struct has padding)
    if (a.m != b.m || a.anm != b.anm || std::memcmp(a.tevColor, b.tevColor, sizeof(a.tevColor)) != 0 ||
        std::memcmp(a.tevK, b.tevK, sizeof(a.tevK)) != 0 || std::memcmp(a.mat, b.mat, sizeof(a.mat)) != 0 ||
        std::memcmp(a.amb, b.amb, sizeof(a.amb)) != 0 || std::memcmp(a.texNo, b.texNo, sizeof(a.texNo)) != 0 ||
        std::memcmp(&a.fog, &b.fog, sizeof(a.fog)) != 0 || std::memcmp(a.texMtxPtr, b.texMtxPtr, sizeof(a.texMtxPtr)) != 0 ||
        std::memcmp(a.texMtx, b.texMtx, sizeof(a.texMtx)) != 0 || std::memcmp(a.coord, b.coord, sizeof(a.coord)) != 0 ||
        std::memcmp(a.cur, b.cur, sizeof(a.cur)) != 0 || std::memcmp(a.shapeCur, b.shapeCur, sizeof(a.shapeCur)) != 0) {
        return false;
    }
    return true;
}
inline u32 ParDlHash(u32 h, const void* p, size_t n) {
    const u8* b = static_cast< const u8* >(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
    return h;
}
// (GXV_PAR_DL=2) one entry's outputs, right after its refresh: its differed lists' bytes (what the front end reads), the texture matrices
// and current-matrix words of its materials (shared with other instances: hence right after it)
// (materials[i] with packets[i], i < n; the game's: a model's data and its material packets)
inline u32 ParDlFingerprint(J3DMaterial* const* materials, J3DMatPacket* const* packets, u32 n) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; ++i) {
        J3DMaterial* m = materials[i];
        h = ParDlHash(h, &m->mCurrentMtx, sizeof(J3DCurrentMtx));
        if (m->getShape() != nullptr) h = ParDlHash(h, &m->getShape()->mCurrentMtx, sizeof(J3DCurrentMtx));
        for (u32 j = 0; j < 8; ++j) {
            if (J3DTexMtx* t = m->getTexGenBlock()->getTexMtx(j)) h = ParDlHash(h, t->getMtx(), sizeof(Mtx));
        }
        J3DMatPacket* mp = packets[i];
        if (mp != nullptr && mp->mpInitShapePacket != nullptr) {
            J3DDisplayListObj* dl = mp->mpInitShapePacket->getDisplayListObj();
            if (dl != nullptr && dl->mpDisplayList[0] != nullptr) {
                h = ParDlHash(h, &dl->mSize, sizeof(dl->mSize));
                h = ParDlHash(h, dl->mpDisplayList[0], dl->mSize);
            }
        }
    }
    return h;
}
// (GXV_PAR_DL=2, fedl25) the same inputs, hashed by part, to name what differs when two fingerprints do: 0 the current-matrix words (materials and
// shapes), 1 the texture matrices, 2 the differed lists' sizes, 3 their bytes (the buffer callDL reads: mpDisplayList[0]), 4 the other buffer's bytes
// (mpDisplayList[1], not in the fingerprint: it shows whether the two buffers of a list hold the same bytes)
inline void ParDlFingerprintParts(J3DMaterial* const* materials, J3DMatPacket* const* packets, u32 n, u32 out[5]) {
    for (u32 k = 0; k < 5; ++k) out[k] = 2166136261u;
    for (u32 i = 0; i < n; ++i) {
        J3DMaterial* m = materials[i];
        out[0] = ParDlHash(out[0], &m->mCurrentMtx, sizeof(J3DCurrentMtx));
        if (m->getShape() != nullptr) out[0] = ParDlHash(out[0], &m->getShape()->mCurrentMtx, sizeof(J3DCurrentMtx));
        for (u32 j = 0; j < 8; ++j) {
            if (J3DTexMtx* t = m->getTexGenBlock()->getTexMtx(j)) out[1] = ParDlHash(out[1], t->getMtx(), sizeof(Mtx));
        }
        J3DMatPacket* mp = packets[i];
        if (mp != nullptr && mp->mpInitShapePacket != nullptr) {
            J3DDisplayListObj* dl = mp->mpInitShapePacket->getDisplayListObj();
            if (dl != nullptr && dl->mpDisplayList[0] != nullptr) {
                out[2] = ParDlHash(out[2], &dl->mSize, sizeof(dl->mSize));
                out[3] = ParDlHash(out[3], dl->mpDisplayList[0], dl->mSize);
                if (dl->mpDisplayList[1] != nullptr) out[4] = ParDlHash(out[4], dl->mpDisplayList[1], dl->mSize);
            }
        }
    }
}
