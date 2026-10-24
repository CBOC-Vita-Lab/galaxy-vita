import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch import sub, done

# Nerve singletons: CodeWarrior needed weak linkage; on Clang the definitions often
# live in anonymous namespaces, where weak is illegal.
sub('include/Game/LiveActor/Nerve.hpp', '#define NEW_NERVE(name, parent_class, executor_name)',
    '#ifdef __MWERKS__\n#define NERVE_WEAK ATTRIBUTE_WEAK\n#else\n#define NERVE_WEAK\n#endif\n\n#define NEW_NERVE(name, parent_class, executor_name)')
sub('include/Game/LiveActor/Nerve.hpp', '    name name::sInstance ATTRIBUTE_WEAK;', '    name name::sInstance NERVE_WEAK;', count=0)

# Class-specific usual deallocation functions must take size_t.
sub('libs/JSystem/include/JSystem/JAudio2/JASHeapCtrl.hpp', 'static void operator delete(void* addr, u32 size) {',
    'static void operator delete(void* addr, size_t size) {', count=0)

# J3DTevsDefault namespace (a CodeWarrior matching hack) -> alias the real globals.
sub('libs/JSystem/include/JSystem/J3DGraphBase/J3DTevs.hpp', '''    extern "C" J3DTevSwapModeInfo j3dDefaultTevSwapMode;
}  // namespace J3DTevsDefault
#endif''', '''    extern "C" J3DTevSwapModeInfo j3dDefaultTevSwapMode;
}  // namespace J3DTevsDefault
#endif
extern const J3DTevSwapModeInfo j3dDefaultTevSwapMode;
#ifndef __MWERKS__
namespace J3DTevsDefault {
    using ::j3dDefaultIndTevStageInfo;
    using ::j3dDefaultTevStageInfo;
    using ::j3dDefaultTevSwapMode;
}  // namespace J3DTevsDefault
#endif''')

# JMath's private "std::pair" collides with libc++'s std::pair.
sub('libs/JSystem/include/JSystem/JMath/JMATrigonometric.hpp', '''namespace std {
    template < typename A1, typename B1 >
    struct pair {
        A1 a1;
        B1 b1;
        pair() {
            a1 = A1();
            b1 = B1();
        }
    };
}  // namespace std''', '''#ifdef __MWERKS__
namespace std {
    template < typename A1, typename B1 >
    struct pair {
        A1 a1;
        B1 b1;
        pair() {
            a1 = A1();
            b1 = B1();
        }
    };
}  // namespace std
#define JMATH_PAIR std::pair
#else
namespace JMath {
    template < typename A1, typename B1 >
    struct TPair {
        A1 a1;
        B1 b1;
        TPair() {
            a1 = A1();
            b1 = B1();
        }
    };
}  // namespace JMath
#define JMATH_PAIR JMath::TPair
#endif''')
sub('libs/JSystem/include/JSystem/JMath/JMATrigonometric.hpp', 'std::pair< T, T > table[LEN];', 'JMATH_PAIR< T, T > table[LEN];')
sub('src/JSystem/JParticle/JPAMath.cpp', 'const std::pair< f32, f32 >* p', 'const JMATH_PAIR< f32, f32 >* p', count=0)

# bionic defines bcopy/bzero as function-like macros.
sub('libs/JSystem/include/JSystem/JAudio2/JASCalc.hpp', '''#include <limits>
#include <revolution.h>
''', '''#include <limits>
#include <revolution.h>
#include <string.h>
#undef bcopy
#undef bzero
''')

done()
