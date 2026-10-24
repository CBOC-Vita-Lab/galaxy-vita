import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch import sub, done

J = 'libs/JSystem/include/JSystem/JMath/JMath.hpp'

sub(J, '''        return out * input;
    } else {
        return input;
    }
#endif
}''', '''        return out * input;
    } else {
        return input;
    }
#else
    if (input > 0.0f) {
        return __builtin_sqrtf(input);
    }
    return input;
#endif
}''')

# Straight transliteration of the paired-single sequence (same operation order).
sub(J, '''        fnmsubs ff25,ff31,ff25,ff26

    }
    // clang-format on
    return ff25;
#endif
}''', '''        fnmsubs ff25,ff31,ff25,ff26

    }
    // clang-format on
    return ff25;
#else
    f32 ff31 = p1 - p2;
    f32 ff30 = p5 - p2;
    f32 ff29 = ff31 / ff30;
    f32 ff28 = ff29 * ff29;
    f32 ff25 = ff29 + ff29;
    f32 ff27 = ff28 - ff29;
    ff30 = p3 - p6;
    f32 ff26 = ff25 * ff27 - ff28;
    ff25 = p4 * ff27 + p4;
    ff26 = ff26 * ff30 + p3;
    ff25 = p7 * ff27 + ff25;
    ff25 = ff29 * p4 - ff25;
    ff25 = ff26 - ff31 * ff25;
    return ff25;
#endif
}''')

sub(J, '''            psq_st x, 0(dest), 0, 0
            stfs y, 8(dest)
        }
#endif
    }''', '''            psq_st x, 0(dest), 0, 0
            stfs y, 8(dest)
        }
#else
        __builtin_memcpy(dest, src, 3 * sizeof(f32));
#endif
    }''')

sub(J, '''            psq_st z, 16(dest), 0, 0
        }
#endif
    }''', '''            psq_st z, 16(dest), 0, 0
        }
#else
        __builtin_memcpy(dest, src, 6 * sizeof(f32));
#endif
    }''')

sub(J, '''                psq_st    f_5, 0x28(pDest), 0, 0
        }
        ;
#endif
    }''', '''                psq_st    f_5, 0x28(pDest), 0, 0
        }
        ;
#else
        __builtin_memcpy(pDest, pSrc, 12 * sizeof(f32));
#endif
    }''')

sub(J, '''                psq_st    f_7, 0x38(pDest), 0, 0
        }
        ;
#endif
    }''', '''                psq_st    f_7, 0x38(pDest), 0, 0
        }
        ;
#else
        __builtin_memcpy(pDest, pSrc, 16 * sizeof(f32));
#endif
    }''')

sub(J, '''#else
    void PSVECCopy(const Vec*, Vec*);
    void PSVECAdd(const Vec*, const Vec*, Vec*);
    void PSVECSubtract(const Vec*, const Vec*, Vec*);
    f32 PSVECDotProduct(const Vec*, const Vec*);
    f32 PSVECSquareMag(const Vec*);
    void PSVECNegate(const Vec*, Vec*);
    f32 PSVECSquareDistance(const Vec*, const Vec*);
    void PSVECMultiply(const Vec*, const Vec*, Vec*);
#endif''', '''#else
    inline f32 PSVECDotProduct(const Vec* pA, const Vec* pB) {
        return (pA->x * pB->x + pA->y * pB->y) + pA->z * pB->z;
    }
    inline void PSVECCopy(const Vec* src, Vec* dest) {
        f32 x = src->x, y = src->y, z = src->z;
        dest->x = x;
        dest->y = y;
        dest->z = z;
    }
    inline void PSVECAdd(const Vec* a, const Vec* b, Vec* dst) {
        f32 x = a->x + b->x, y = a->y + b->y, z = a->z + b->z;
        dst->x = x;
        dst->y = y;
        dst->z = z;
    }
    inline void PSVECSubtract(const Vec* a, const Vec* b, Vec* dst) {
        f32 x = a->x - b->x, y = a->y - b->y, z = a->z - b->z;
        dst->x = x;
        dst->y = y;
        dst->z = z;
    }
    inline void PSVECMultiply(const Vec* a, const Vec* b, Vec* dst) {
        f32 x = a->x * b->x, y = a->y * b->y, z = a->z * b->z;
        dst->x = x;
        dst->y = y;
        dst->z = z;
    }
    inline f32 PSVECSquareMag(const Vec* src) {
        return (src->x * src->x + src->y * src->y) + src->z * src->z;
    }
    inline void PSVECNegate(const Vec* src, Vec* dst) {
        f32 x = -src->x, y = -src->y, z = -src->z;
        dst->x = x;
        dst->y = y;
        dst->z = z;
    }
    inline f32 PSVECSquareDistance(const Vec* a, const Vec* b) {
        f32 dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
        return (dx * dx + dy * dy) + dz * dz;
    }
#endif''')

T = 'libs/JSystem/include/JSystem/JGeometry/TVec.hpp'

sub(T, '''#else
    inline void negateInternal(const f32* rSrc, f32* rDest);
#endif''', '''#else
    inline void negateInternal(const f32* rSrc, f32* rDest) {
        f32 x = -rSrc[0], y = -rSrc[1], z = -rSrc[2];
        rDest[0] = x;
        rDest[1] = y;
        rDest[2] = z;
    }
#endif''')

sub(T, '''#else
    static void subInternal(const f32* vec1, const f32* vec2, f32* dst);
#endif''', '''#else
    inline static void subInternal(const f32* vec1, const f32* vec2, f32* dst) {
        f32 x = vec1[0] - vec2[0], y = vec1[1] - vec2[1], z = vec1[2] - vec2[2];
        dst[0] = x;
        dst[1] = y;
        dst[2] = z;
    }
#endif''')

sub(T, '''#else
    void mulInternal(const f32* vec1, const f32* vec2, f32* dst);
#endif''', '''#else
    inline void mulInternal(const f32* vec1, const f32* vec2, f32* dst) {
        f32 x = vec1[0] * vec2[0], y = vec1[1] * vec2[1], z = vec1[2] * vec2[2];
        dst[0] = x;
        dst[1] = y;
        dst[2] = z;
    }
#endif''')

sub(T, '''#else
        TVec3(const Vec& vec);
#endif''', '''#else
        TVec3(const Vec& vec) {
            x = vec.x;
            y = vec.y;
            z = vec.z;
        }
#endif''')

sub(T, '''#else
        TVec3(const TVec3< f32 >& vec);
#endif''', '''#else
        TVec3(const TVec3< f32 >& vec) : Vec() {
            x = vec.x;
            y = vec.y;
            z = vec.z;
        }
#endif''')

sub(T, '''            psq_l a_x, 0(v_a), 0, 0
            lfs b_x, 8(v_a)
            psq_st a_x, 0(v_b), 0, 0
            stfs b_x, 8(v_b)
            }
            ;
#endif
        }''', '''            psq_l a_x, 0(v_a), 0, 0
            lfs b_x, 8(v_a)
            psq_st a_x, 0(v_b), 0, 0
            stfs b_x, 8(v_b)
            }
            ;
#else
            f32 nx = vec.x, ny = vec.y, nz = vec.z;
            x = nx;
            y = ny;
            z = nz;
#endif
        }''')

sub(T, '''#else
        void setPSZeroVec();
#endif''', '''#else
        void setPSZeroVec() {
            x = gZeroVec.x;
            y = gZeroVec.y;
            z = gZeroVec.z;
        }
#endif''')

sub(T, '''                ps_madd  _fp3, _fp5, _fp4, _fp2;
                ps_sum0  _fp1, _fp3, _fp2, _fp2;
            }

            return _fp1;
#endif
        }''', '''                ps_madd  _fp3, _fp5, _fp4, _fp2;
                ps_sum0  _fp1, _fp3, _fp2, _fp2;
            }

            return _fp1;
#else
            return (x * rOther.x + y * rOther.y) + z * rOther.z;
#endif
        }''')

sub(T, '''#else
        f32 squared(const TVec3& rB) const;
#endif''', '''#else
        f32 squared(const TVec3& rB) const {
            f32 dx = x - rB.x, dy = y - rB.y, dz = z - rB.z;
            return (dx * dx + dy * dy) + dz * dz;
        }
#endif''')

done()
