import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch import sub, done

M = 'src/Game/Util/MathUtil.cpp'

sub(M, '''        psq_st     f5, 0(pDst),  1, 0
        psq_st     f2, 4(pDst),  0, 0
    }  // clang-format on
    return dot;
#endif
}''', '''        psq_st     f5, 0(pDst),  1, 0
        psq_st     f2, 4(pDst),  0, 0
    }  // clang-format on
    return dot;
#else
    f32 dot = (pSrc->x * pKill->x + pSrc->y * pKill->y) + pSrc->z * pKill->z;
    f32 x = pSrc->x - dot * pKill->x;
    f32 y = pSrc->y - dot * pKill->y;
    f32 z = pSrc->z - dot * pKill->z;
    Vec* pOut = const_cast< Vec* >(pDst);
    pOut->x = x;
    pOut->y = y;
    pOut->z = z;
    return dot;
#endif
}''')

sub(M, '''            ps_madds0 f0, f3, a3, f0
            ps_madds0 f2, f4, a3, f2
            psq_st f0, 0(pA1), 0, 0
            psq_st f2, 8(pA1), 1, 0
        }

#endif
    }''', '''            ps_madds0 f0, f3, a3, f0
            ps_madds0 f2, f4, a3, f2
            psq_st f0, 0(pA1), 0, 0
            psq_st f2, 8(pA1), 1, 0
        }

#else
        TVec3f* pOut = const_cast< TVec3f* >(pA1);
        pOut->x = pA2->x * a3 + pA1->x;
        pOut->y = pA2->y * a3 + pA1->y;
        pOut->z = pA2->z * a3 + pA1->z;
#endif
    }''')

sub(M, '''            psq_st    f4, 0(pA3), 0, 0
            psq_st    f3, 8(pA3), 1, 0
        }

#endif
    }''', '''            psq_st    f4, 0(pA3), 0, 0
            psq_st    f3, 8(pA3), 1, 0
        }

#else
        f32 x = pA2->x * a5 + pA1->x * a4;
        f32 y = pA2->y * a5 + pA1->y * a4;
        f32 z = pA2->z * a5 + pA1->z * a4;
        pA3->x = x;
        pA3->y = y;
        pA3->z = z;
#endif
    }''')

sub(M, '''    __REGISTER f32 inverse;
    __asm { frsqrte inverse, value }
    f32 estimate = inverse * value;
    inverse = -(estimate * inverse - 3.0f);
    inverse *= estimate;
    inverse *= 0.5f;
    return inverse;
}''', '''#ifdef __MWERKS__
    __REGISTER f32 inverse;
    __asm { frsqrte inverse, value }
    f32 estimate = inverse * value;
    inverse = -(estimate * inverse - 3.0f);
    inverse *= estimate;
    inverse *= 0.5f;
    return inverse;
#else
    return __builtin_sqrtf(value);
#endif
}''')

A = 'src/JSystem/J3DGraphAnimator/J3DAnimation.cpp'
sub(A, '''        fmadds value, delta, squared, value
        fsubs value, value, end
    }

    return value;
#endif
}''', '''        fmadds value, delta, squared, value
        fsubs value, value, end
    }

    return value;
#else
    // Same sequence as the paired-single code; GQR5 loads s16 with no scaling.
    f32 value = pp1;
    f32 time = (f32)*pp2;
    f32 end = (f32)*pp5;
    f32 start = (f32)*pp3;
    f32 duration = end - time;
    end = (f32)*pp6;
    f32 t = value - time;
    value = (f32)*pp7;
    f32 delta = end - start;
    t = t / duration;
    time = (f32)*pp4;
    value = value * duration + start;
    delta = delta - duration * time;
    f32 squared = t * t;
    value = value - end;
    value = value - delta;
    end = squared * value;
    value = duration * time + end;
    value = value * t + start;
    value = delta * squared + value;
    value = value - end;
    return value;
#endif
}''')

J = 'src/JSystem/JMath/JMath.cpp'
sub(J, '''        ps_sum0     dp, dp, dp, dp
    }
#endif  // clang-format on''', '''        ps_sum0     dp, dp, dp, dp
    }
#else
    dp = (p->z * q->z + p->x * q->x) + (p->w * q->w + p->y * q->y);
#endif  // clang-format on''')

sub(J, '''        psq_l v2z, 8(vec2), 1, 0
        ps_madds0 rz, v1z,  scale, v2z
        psq_st rz, 8(dst), 1, 0
	}
#endif  // clang-format on
}''', '''        psq_l v2z, 8(vec2), 1, 0
        ps_madds0 rz, v1z,  scale, v2z
        psq_st rz, 8(dst), 1, 0
	}
#else
    f32 x = vec1->x * scale + vec2->x;
    f32 y = vec1->y * scale + vec2->y;
    f32 z = vec1->z * scale + vec2->z;
    dst->x = x;
    dst->y = y;
    dst->z = z;
#endif  // clang-format on
}''')

sub(J, '''        psq_st bxy, 0(dst), 0, 0
        stfs bz, 8(dst)
    }
#endif
}''', '''        psq_st bxy, 0(dst), 0, 0
        stfs bz, 8(dst)
    }
#else
    f32 x = (b->x - a->x) * t + a->x;
    f32 y = (b->y - a->y) * t + a->y;
    f32 z = (b->z - a->z) * t + a->z;
    dst->x = x;
    dst->y = y;
    dst->z = z;
#endif
}''')

S = 'src/JSystem/J3DGraphBase/J3DStruct.cpp'
sub(S, '''    mEffectMtx[3][2] = kIdentityZero;
    mEffectMtx[3][3] = kIdentityW;
#endif
}''', '''    mEffectMtx[3][2] = kIdentityZero;
    mEffectMtx[3][3] = kIdentityW;
#else
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            mEffectMtx[r][c] = param_0[r][c];
        }
    }
    mEffectMtx[3][0] = 0.0f;
    mEffectMtx[3][1] = 0.0f;
    mEffectMtx[3][2] = 0.0f;
    mEffectMtx[3][3] = 1.0f;
#endif
}''')

done()
