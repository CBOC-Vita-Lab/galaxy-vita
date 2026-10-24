import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch import read, write, sub, done

sub('include/math_types.hpp', '''namespace std {
    inline f32 atan2(f32 x, f32 y) {
        return ::atan2(x, y);
    }
};  // namespace std''', '''#ifdef __MWERKS__
namespace std {
    inline f32 atan2(f32 x, f32 y) {
        return ::atan2(x, y);
    }
};  // namespace std
#endif''')

sub('src/JSystem/JUtility/JUTException.cpp', '''    panic_f_va(file, line, format, args);
    va_end();''', '''    panic_f_va(file, line, format, args);
    va_end(args);''')
sub('src/Game/Scene/IntermissionScene.cpp', '''    vsnprintf(mState, sizeof(mState), pState, list);
    va_end();''', '''    vsnprintf(mState, sizeof(mState), pState, list);
    va_end(list);''')

sub('libs/JSystem/include/JSystem/JMath/JMATrigonometric.hpp', '''    template < s32 Len, typename T >
    class TAsinAcosTable {''', '''#ifndef __MWERKS__
    template <>
    f32 TAtanTable< 1024, f32 >::atan2_(f32 y, f32 x) const;
#endif

    template < s32 Len, typename T >
    class TAsinAcosTable {''')

sub('src/JSystem/J3DGraphBase/J3DShape.cpp', 'GDSetArrayRaw((GXAttr)(i + GX_VA_POS), nullptr, stride[i]);', 'GDSetArrayRaw((GXAttr)(i + GX_VA_POS), 0, stride[i]);')

sub('src/Game/Util/LayoutUtil.cpp', '''template nw4r::lyt::PaneList::Iterator nw4r::ut::LinkList< nw4r::lyt::Pane, 4 >::Iterator::operator++(int);''', '''#ifdef __MWERKS__
template nw4r::lyt::PaneList::Iterator nw4r::ut::LinkList< nw4r::lyt::Pane, 4 >::Iterator::operator++(int);
#endif''')

sub('src/Game/Util/HashUtil.cpp', '''inline int tolower(int c) {
    return ((c < 0) || (c >= 0x100)) ? c : (int)(_current_locale.ctype_cmpt_ptr->lower_map_ptr[c]);
}''', '''#ifdef __MWERKS__
inline int tolower(int c) {
    return ((c < 0) || (c >= 0x100)) ? c : (int)(_current_locale.ctype_cmpt_ptr->lower_map_ptr[c]);
}
#else
#include <ctype.h>
#endif''')

sub('src/Game/Util/DemoUtil.cpp', '''    LiveActor* getTalkingActor() {
        if (isExistSceneObj(SceneObj_TalkDirector) == false) {
            return false;
        }''', '''    LiveActor* getTalkingActor() {
        if (isExistSceneObj(SceneObj_TalkDirector) == false) {
            return nullptr;
        }''')

sub('src/Game/System/ResourceHolderManager.cpp', 'MR::Functor(SingletonHolder< ResourceHolderManager >::get(), createResourceHolder, pParam1, pArgs)',
    'MR::Functor(SingletonHolder< ResourceHolderManager >::get(), &ResourceHolderManager::createResourceHolder, pParam1, pArgs)')
sub('src/Game/System/ResourceHolderManager.cpp', 'MR::Functor(SingletonHolder< ResourceHolderManager >::get(), createLayoutHolder, pParam1, pArgs)',
    'MR::Functor(SingletonHolder< ResourceHolderManager >::get(), &ResourceHolderManager::createLayoutHolder, pParam1, pArgs)')

sub('src/Game/System/GameSystemStationedArchiveLoader.cpp', 'napa->alloc(0x10000, nullptr);', 'napa->alloc(0x10000, 0);', count=0)

sub('src/Game/Screen/GalaxyMapController.cpp', 'MR::Functor(this, GalaxyMapController::capture)', 'MR::Functor(this, &GalaxyMapController::capture)')

rtp = 'src/Game/Screen/ReplaceTagProcessor.cpp'
sub(rtp, '''        va_list copy;
        const wchar_t* pString = nullptr;
        *copy = *args;''', '''        va_list copy;
        const wchar_t* pString = nullptr;
        va_copy(copy, args);''')
sub(rtp, '''        va_list copy;
        int number = 0;
        *copy = *args;''', '''        va_list copy;
        int number = 0;
        va_copy(copy, args);''')

sub('src/Game/Player/MarioWarp.cpp', '''namespace JGeometry {
    TVec3< f32 > TVec3< f32 >::operator*(f32) const NO_INLINE;
}''', '''#ifdef __MWERKS__
namespace JGeometry {
    TVec3< f32 > TVec3< f32 >::operator*(f32) const NO_INLINE;
}
#endif''')

sub('src/Game/NPC/NPCActor.cpp', 'MR::createJointDelegatorWithNullChildFunc(this, &calcJointScale, rCaps._70)',
    'MR::createJointDelegatorWithNullChildFunc(this, &NPCActor::calcJointScale, rCaps._70)')

# The original evaluates a member function's address here (always true).
sub('src/Game/MapObj/Sandstorm.cpp', '        if (isSunakazeKun) {', '        if (true /* original tests the member function address */) {')
sub('src/Game/MapObj/MapPartsRailPosture.cpp', 'if (mMovePosture == 1 || isPostureTypeRailDirRailUseShadowGravity) {',
    'if (mMovePosture == 1 || true /* original tests the member function address */) {')

tb = 'src/Game/NPC/TalkBalloon.cpp'
t = read(tb)
if 'tbFmin' not in t:
    t = t.replace('inline f32 fmin(f32 a, f32 b) {', 'inline f32 tbFmin(f32 a, f32 b) {')
    t = t.replace('inline f32 fmax(f32 a, f32 b) {', 'inline f32 tbFmax(f32 a, f32 b) {')
    import re
    t = re.sub(r'(?<![\w:.])fmin\(', 'tbFmin(', t)
    t = re.sub(r'(?<![\w:.])fmax\(', 'tbFmax(', t)
    t = t.replace('inline f32 tbtbFmin', 'inline f32 tbFmin').replace('inline f32 tbtbFmax', 'inline f32 tbFmax')
    write(tb, t)

sub('src/Game/MapObj/MapPartsBreaker.cpp', 'f32 TVec2f::squared(const TVec2f& rOther) const {', 'template <>\nf32 TVec2f::squared(const TVec2f& rOther) const {')

wp = 'src/Game/Map/WaterPlant.cpp'
for n in 'ABCD':
    sub(wp, 'new JUTTexture(MR::loadTexFromArc("WaterPlant.arc", "WaterPlant%s.bti"), nullptr);' % n,
        'new JUTTexture(MR::loadTexFromArc("WaterPlant.arc", "WaterPlant%s.bti"), 0);' % n)

sub('src/Game/Enemy/Mogucchi.cpp', 'MR::calcGravityVector(this, MR::getRailPos(this), &mRailGravity, nullptr, nullptr);',
    'MR::calcGravityVector(this, MR::getRailPos(this), &mRailGravity, nullptr, 0);')

sub('src/Game/Effect/AutoEffectInfo.cpp', 'extern "C" u32 strtoul(const char*, char**, int);', '#ifdef __MWERKS__\nextern "C" u32 strtoul(const char*, char**, int);\n#else\n#include <stdlib.h>\n#endif')

mo = 'include/Game/MapObj/MorphItemObjNeo.hpp'
sub(mo, '''    virtual TVec3f* getClippingCenterOffset() const {
        return &(TVec3f(0.0f, 200.0f, 0.0f));
    }''', '''    virtual TVec3f* getClippingCenterOffset() const {
        static TVec3f sOffset(0.0f, 200.0f, 0.0f);
        return &sOffset;
    }''')
sub(mo, '''    virtual TVec3f* getClippingCenterOffset() const {
        return &(TVec3f(0.0f, 200.0f, 0.0f));
    };''', '''    virtual TVec3f* getClippingCenterOffset() const {
        static TVec3f sOffset(0.0f, 200.0f, 0.0f);
        return &sOffset;
    };''')
sub(mo, '''    virtual TVec3f* getClippingCenterOffset() const {
        return &TVec3f(0.0f, 580.0f, 0.0f);
    }''', '''    virtual TVec3f* getClippingCenterOffset() const {
        static TVec3f sOffset(0.0f, 580.0f, 0.0f);
        return &sOffset;
    }''')
sub('src/Game/Effect/EffectObjGravityDust.cpp', '''    return &(TVec3f(0.0f, 500.0f * mScale.y, 0.0f));''', '''    static TVec3f sOffset;
    sOffset.set(0.0f, 500.0f * mScale.y, 0.0f);
    return &sOffset;''')
sub('src/Game/MapObj/PlantGroup.cpp', '''        const TVec3f* pUp = &(rGravity * 100.0f);''', '''        TVec3f up = rGravity * 100.0f;
        const TVec3f* pUp = &up;''')
sub('src/Game/MapObj/PlantGroup.cpp', '''        const TVec3f* pRay = &(rGravity * ::sCheckLineLength);''', '''        TVec3f ray = rGravity * ::sCheckLineLength;
        const TVec3f* pRay = &ray;''')

sub('src/Game/System/GameSystem.cpp', 'void main(void) {', '#ifdef TARGET_PC\nvoid port_game_main(void) {\n#else\nvoid main(void) {\n#endif')

# Explicit specializations must precede first use: move them to the top of the file.
d = 'src/Game/Demo/DemoStartRequestHolder.cpp'
t = read(d)
start = t.index('MR::FixedRingBuffer< const DemoStartInfo*, 16 >::iterator::iterator(const DemoStartInfo** pHead')
start = t.rindex('\n', 0, start) + 1
# include any template<> line right above
prev = t.rindex('\n', 0, start - 1) + 1
if t[prev:start].strip() == 'template <>':
    start = prev
end = t.index('void MR::FixedRingBuffer< const DemoStartInfo*, 16 >::iterator::operator++()')
end = t.index('\n}\n', end) + 3
block = t[start:end]
if t.index('#include <revolution/types.h>\n') < start and 'DemoStartRequestHolder::DemoStartRequestHolder()' in t[:start]:
    t = t[:start] + t[end:]
    anchor = '#include <revolution/types.h>\n'
    i = t.index(anchor) + len(anchor)
    t = t[:i] + '\n' + block + '\n' + t[i:]
    write(d, t)

done()
