#pragma once

#include <revolution/gx/GXEnum.h>

class BloomEffect;
class JUTTexture;
class NameObj;

namespace ImageEffectLocalUtil {
    enum ETexDrawType {
        TexDrawType_0,
        TexDrawType_1,
        TexDrawType_2,
        TexDrawType_3,
    };

    void capture(JUTTexture* pTexture, s32 divisions, s32 tile, GXTexFmt format, bool clear, u8 filter);
    void setupDrawTexture();
    void drawTexture(JUTTexture* pTexture, s32 divisions, s32 tile, u8 intensity, ETexDrawType texDrawType);
    void sendTextureVertex(s32 divisions, s32 tile);
    void blurTexture(JUTTexture* pTexture, s32 divisions, s32 tile, u32 sampleCount, f32 radius, f32 intensity);
#ifdef TARGET_PC
    // PETARI_BLOOM_HALFRATE (gpu23, ImageEffectLocalUtil.cpp): the bloom's glow made every other picture.
    void portPictureTick();                  // once a picture the game draws (GameSystem::draw, before the scene)
    // dive27 (GXV_REHEARSE): while true the bloom chain is being rehearsed in a warm-up scope behind the closed wipe (BloomEffect::portRehearse): the skip / reuse / census bookkeeping
    // of the real pictures is left alone (they answer "full chain, nothing remembered").
    bool portRehearsing();
    void portRehearseSet(bool on);
    bool portBloomReuse(const void* pOwner);  // true: this picture adds the glow of the previous one (no captures, no passes)
    bool portBloomReusingNow();               // the last answer of this picture (false when nothing asked)
    void portBloomTexturesTouched();          // another image effect wrote the shared textures: the next bloom makes its glow
    u32 portPictureSerial();                  // the picture being drawn (portPictureTick's count)
    // PETARI_SHADOWTEX_HALFRATE (gpu23, MarioShadow.cpp): true when this picture may keep the player's shadow texture of the previous
    // picture (owner: the CollisionShadow); false: draw and capture it now (and remember this picture).
    bool portShadowTexReuse(const void* pOwner);
    bool portClipChainReuse(const void* pOwner);  // (PETARI_CLIPCHAIN_HALFRATE) true: the clip field's soft edge is drawn from the previous picture's chain
    // PETARI_CLIPCHAIN_ONEPASS / PETARI_CLIPCHAIN_SKIPEMPTY (clipblur25, ImageEffectLocalUtil.cpp): the clip field's soft edge in one draw.
    int portClipOnePassMode();                                          // 0 off, 1 the edge from the mask in one draw, 2 one tile draw + one capture
    int portClipTapsSetup(JUTTexture* pTexture, bool inside, int extraStages);  // K compare-accumulate stages over pTexture; returns K
    void portClipTapsEnd();                                             // the one-stage state setupDrawTexture leaves
    void portClipVolumeDrawn();                                         // ClipArea::draw: a volume went into the alpha plane this picture
    bool portClipSkipEmpty(bool inside);                                // true: no volume drew (and the switch is on): FallOutFieldDraw draws nothing
    void portClipSetDry(bool dry);                                      // capture() and sendTextureVertex() send nothing (the GX state calls are still made)
    bool portClipDry();
    void portClipNote(bool onePass, bool skipped);                      // census line every 3600 pictures
    // PETARI_BLOOM_SKIP (revisit23, ImageEffectLocalUtil.cpp): BloomEffect's glow chain left out on a picture where no glow source drew.
    void portBloomFillDone(const void* pOwner, bool wholeEfb);  // preDraw, right after its black fill (wholeEfb: viewport and scissor cover the EFB)
    bool portBloomNothingDrawn(const void* pOwner, bool wholeEfb);  // postDraw: true = make the chain "dry" (state only; the glow is black)
    bool portBloomGlowBlack(const void* pOwner);     // the glow kept from the last chain of pOwner is black (PETARI_BLOOM_HALFRATE's reuse adds nothing)
    // GXV_BLOOM_BRACKET (bloom37, ImageEffectLocalUtil.cpp, BloomEffect.cpp): the scene bracket (capture, black fill, put-back) of a bloom picture where no glow source drew.
    // 0 off, 1 on (the chain is made dry and the vgx front end may leave the bracket out), 2 count only; read once.
    int portBloomBracketMode();
    void portBloomBracketMark(unsigned sub);  // PORT_GX_MARK_BLOOM_BRACKET with this sub-kind (PORT_BB_*): nothing with the switch off or while rehearsing
    bool portBloomBracketDead();              // the last portBloomNothingDrawn found the bracket dead (nothing was sent between the fill and postDraw; both cover the EFB)
    // GXV_BLOOM_FAST (bloom23, BloomEffect.cpp / BloomEffectSimple.cpp): 1 = the fast chains, 2 = the exact cut only, 0 = the original
    // chains (0 / 1 / 2 / perf / exact, read once; its start line with the first bloom picture).
    int portBloomFastMode();
    // A bloom picture's census: what its chain sent (captures and draws; kind 0 normal original, 1 normal fast, 2 normal reused
    // (PETARI_BLOOM_HALFRATE), 3 normal dry (PETARI_BLOOM_SKIP), 4 simple original, 5 simple fast, 6 simple exact, 7 simple reused).
    // The "bloom-fast:" basic-log line every 600 bloom pictures (or 1800 pictures).
    void portBloomChainNote(const void* pOwner, u32 captures, u32 draws, int kind);
    // GXV_BLOOM_FOLD (bloomfold25, ImageEffectLocalUtil.cpp, BloomEffect.cpp, BloomEffectSimple.cpp): 0 off, 1 folded chains, 2 reduced glow; read through the
    // stage knobs. The helpers draw the folds' passes (see the comment in ImageEffectLocalUtil.cpp); census kinds 8 / 9 normal fold 1 / 2, 10 / 11 simple.
    int portBloomFoldMode();
    int portBloomFoldModeNormal();  // (dive27 GXV_WATER_BLOOMFOLD: the normal bloom's level: portBloomFoldMode(), raised in the underwater-bloom stages)
    struct FoldTap {
        f32 du, dv;  // the texture coordinate offset of the tap (in the coordinates of the texture, 0..1 over the picture)
        u8 w;        // weight / 255
    };
    void portFoldPairTaps(JUTTexture* pTex, const FoldTap* pTaps, u32 pairs, u8 preK, u32 preScale, bool blend, bool dry, f32 left, f32 top, f32 right,
                          f32 bottom);
    void portFoldSumTaps(JUTTexture* pTex, const FoldTap* pTaps, u32 count, bool preClamp, u8 preK, u32 preScale, bool blend, bool dry, f32 left, f32 top,
                         f32 right, f32 bottom);
    void portFoldGatedTaps(JUTTexture* pTex, const FoldTap* pTaps, u32 count, s32 maskFilter, u8 threshold, bool blend, bool dry, f32 left, f32 top, f32 right,
                           f32 bottom);
    void portFoldGatedBox(JUTTexture* pTex, const FoldTap& rTap, u8 threshold, bool blend, bool dry, f32 left, f32 top, f32 right, f32 bottom);
    void portFoldGlow(JUTTexture* pTex, u8 intensity, bool scale2, bool dry);
    void portFoldEnd();
#endif
};  // namespace ImageEffectLocalUtil

namespace MR {
    void connectToSceneImageEffect(NameObj* pObj);
    void connectToSceneImageEffectMovement(NameObj* pObj);
    void connectToSceneNormalBloom(BloomEffect* pBloomEffect);
};  // namespace MR
