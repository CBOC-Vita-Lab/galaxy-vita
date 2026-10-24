import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch import sub, done

# nw4r inline asm -> portable C
sub('libs/nw4r/include/nw4r/math/arithmetic.h', '''        inline f32 FAbs(register f32 x) {
            register f32 ax;

            __asm {
                fabs ax, x
            }
            ;

            return ax;
        }

        inline f32 FSelect(register f32 cond, register f32 ifPos, register f32 ifNeg) {
            register f32 ret;
            asm { fsel   ret, cond, ifPos, ifNeg }
            return ret;
        }''', '''#ifdef __MWERKS__
        inline f32 FAbs(register f32 x) {
            register f32 ax;

            __asm {
                fabs ax, x
            }
            ;

            return ax;
        }

        inline f32 FSelect(register f32 cond, register f32 ifPos, register f32 ifNeg) {
            register f32 ret;
            asm { fsel   ret, cond, ifPos, ifNeg }
            return ret;
        }
#else
        inline f32 FAbs(f32 x) {
            return __builtin_fabsf(x);
        }

        inline f32 FSelect(f32 cond, f32 ifPos, f32 ifNeg) {
            return cond >= 0.0f ? ifPos : ifNeg;
        }
#endif''')

sub('libs/nw4r/include/nw4r/ut/LinkList.h', 'friend class Self;', 'friend Self;', count=0)

sub('libs/RVL_SDK/include/revolution/mem/heapCommon.h', 'typedef u32 UIntPtr;', 'typedef __UINTPTR_TYPE__ UIntPtr;')

sub('libs/RVL_SDK/include/revolution/os.h', '#include <cstdarg>\n', '#ifdef __cplusplus\n#include <cstdarg>\n#else\n#include <stdarg.h>\n#endif\n')
sub('libs/RVL_SDK/include/revolution/mem/list.h', '#include <cstddef>\n', '#include <stddef.h>\n')

sub('libs/JSystem/include/JSystem/J3DGraphBase/J3DTevs.hpp', '''// doing this matches. figure out why
namespace J3DTevsDefault {
    extern "C" J3DIndTevStageInfo j3dDefaultIndTevStageInfo;
    extern "C" J3DTevStageInfo j3dDefaultTevStageInfo;
    extern "C" J3DTevSwapModeInfo j3dDefaultTevSwapMode;
}  // namespace J3DTevsDefault''', '''#ifdef __MWERKS__
// doing this matches. figure out why
namespace J3DTevsDefault {
    extern "C" J3DIndTevStageInfo j3dDefaultIndTevStageInfo;
    extern "C" J3DTevStageInfo j3dDefaultTevStageInfo;
    extern "C" J3DTevSwapModeInfo j3dDefaultTevSwapMode;
}  // namespace J3DTevsDefault
#endif''')

sub('libs/JSystem/include/JSystem/JGadget/linklist.hpp', '''namespace std {

    struct input_iterator_tag {};''', '''#ifndef __MWERKS__
#include <iterator>
#else
namespace std {

    struct input_iterator_tag {};''')
sub('libs/JSystem/include/JSystem/JGadget/linklist.hpp', '''    };

}  // namespace std
''', '''    };

}  // namespace std
#endif
''')

sub('libs/JSystem/include/JSystem/JGeometry/TVec.hpp', '''        TVec3< T >(int x, int y, int Z);
''', '''#ifdef __MWERKS__
        TVec3< T >(int x, int y, int Z);
#endif
''')
sub('libs/JSystem/include/JSystem/JGeometry/TVec.hpp', '''                set< f32 >(0.0f, 0.0f, 0.0f, 1.0f);''', '''                this->template set< f32 >(0.0f, 0.0f, 0.0f, 1.0f);''')

sub('libs/JSystem/include/JSystem/JGeometry/TMatrix.hpp', '''            rDst.set< f32 >(rSrc.x * get(0, 0) + rSrc.y * get(0, 1) + rSrc.z * get(0, 2),

                            rSrc.x * get(1, 0) + rSrc.y * get(1, 1) + rSrc.z * get(1, 2),

                            rSrc.x * get(2, 0) + rSrc.y * get(2, 1) + rSrc.z * get(2, 2));''', '''            rDst.template set< f32 >(rSrc.x * this->get(0, 0) + rSrc.y * this->get(0, 1) + rSrc.z * this->get(0, 2),

                            rSrc.x * this->get(1, 0) + rSrc.y * this->get(1, 1) + rSrc.z * this->get(1, 2),

                            rSrc.x * this->get(2, 0) + rSrc.y * this->get(2, 1) + rSrc.z * this->get(2, 2));''')

sub('libs/JSystem/include/JSystem/JAudio2/JASGadget.hpp', 'JASPtrArray() : JASPtrTable(mPtrArray, LEN) {', 'JASPtrArray() : JASPtrTable< T >(mPtrArray, LEN) {')

sub('libs/JSystem/include/JSystem/JAudio2/JASSeqReader.hpp', 'return (u32)mSeqCursor - (u32)mSeqBuff;', 'return (u32)(mSeqCursor - mSeqBuff);')

sub('libs/JSystem/include/JSystem/JAudio2/JAISeMgr.hpp', '''    void JAISeMgr_appendSe_(JAISe* se) {
        mSeList.append(se);
    }''', '''    // Template so the JAISe -> JSULink<JAISe> conversion is checked where JAISe is complete.
    template < typename TSe >
    void JAISeMgr_appendSe_(TSe* se) {
        mSeList.append(static_cast< JSULink< TSe >* >(se));
    }''')

sub('include/Game/LiveActor/ShadowVolumeDrawer.hpp', '''    virtual void loadModelDrawMtx() const override;
    virtual void drawShape() const override;''', '''    virtual void loadModelDrawMtx() const;
    virtual void drawShape() const;''')

done()
