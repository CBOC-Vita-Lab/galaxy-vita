import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch import sub, done

# MR::frsqrte(x) returns (1/sqrt(x)) * x, i.e. an estimate of sqrt(x).
sub('include/Game/Util/MathUtil.hpp', '''#else
    f32 frsqrte(f32);
    f32 fastSqrtf(f32);
#endif''', '''#else
    inline f32 frsqrte(f32 x) {
        return __builtin_sqrtf(x);
    }

    inline f32 fastSqrtf(f32 x) {
        if (x > 0.0f) {
            return __builtin_sqrtf(x);
        }

        return x;
    }
#endif''')

sub('libs/JSystem/include/JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp', '''            psq_st u, 24(destination), 0, 0
            stfs v, 32(destination)
        }
#endif
    }''', '''            psq_st u, 24(destination), 0, 0
            stfs v, 32(destination)
        }
#else
        Mtx33& dst = *mpNrmMtxArr[1][mCurrentViewNo][idx];
        for (int r = 0; r < 3; r++) {
            dst[r][0] = mtx[r][0];
            dst[r][1] = mtx[r][1];
            dst[r][2] = mtx[r][2];
        }
#endif
    }''')

sub('libs/JSystem/include/JSystem/J3DGraphBase/J3DDrawBuffer.hpp', '''        ps_sum0 out, out, out, out
    }
    // clang-format on

    return out;
#endif
}''', '''        ps_sum0 out, out, out, out
    }
    // clang-format on

    return out;
#else
    return (v.z * m[2][2] + v.x * m[2][0]) + (m[2][3] + v.y * m[2][1]);
#endif
}''')

sub('libs/JSystem/include/JSystem/J3DGraphBase/J3DStruct.hpp', '''        asm {
            psq_l xy, 0(src), 0, 0
            psq_st xy, 0(dst), 0, 0
        }
        ;
#endif
    }
};  // Size: 0x14''', '''        asm {
            psq_l xy, 0(src), 0, 0
            psq_st xy, 0(dst), 0, 0
        }
        ;
#else
        mScaleX = other.mScaleX;
        mScaleY = other.mScaleY;
        mRotation = other.mRotation;
        mTranslationX = other.mTranslationX;
        mTranslationY = other.mTranslationY;
#endif
    }
};  // Size: 0x14''')

done()
