import sys, os, re; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch import read, write, sub, done

F = 'src/JSystem/J3DGraphBase/J3DTransform.cpp'

C_IMPLS = {
    'J3DPSCalcInverseTranspose': '''void J3DPSCalcInverseTranspose(Mtx src, Mtx33 dst) {
    // Inverse-transpose of the upper 3x3 (cofactor matrix / determinant).
    f32 c00 = src[1][1] * src[2][2] - src[2][1] * src[1][2];
    f32 c01 = src[1][2] * src[2][0] - src[2][2] * src[1][0];
    f32 c02 = src[1][0] * src[2][1] - src[1][1] * src[2][0];
    f32 c10 = src[2][1] * src[0][2] - src[0][1] * src[2][2];
    f32 c11 = src[2][2] * src[0][0] - src[0][2] * src[2][0];
    f32 c12 = src[0][1] * src[2][0] - src[0][0] * src[2][1];
    f32 c20 = src[0][1] * src[1][2] - src[1][1] * src[0][2];
    f32 c21 = src[0][2] * src[1][0] - src[1][2] * src[0][0];
    f32 c22 = src[0][0] * src[1][1] - src[0][1] * src[1][0];
    f32 det = src[0][0] * c00 + src[1][0] * c10 + src[2][0] * c20;
    if (det == 0.0f) {
        return;
    }
    f32 inv = 1.0f / det;
    dst[0][0] = c00 * inv;
    dst[0][1] = c01 * inv;
    dst[0][2] = c02 * inv;
    dst[1][0] = c10 * inv;
    dst[1][1] = c11 * inv;
    dst[1][2] = c12 * inv;
    dst[2][0] = c20 * inv;
    dst[2][1] = c21 * inv;
    dst[2][2] = c22 * inv;
}''',
    'J3DScaleNrmMtx': '''void J3DScaleNrmMtx(Mtx mtx, const Vec& scl) {
    for (int r = 0; r < 3; r++) {
        mtx[r][0] *= scl.x;
        mtx[r][1] *= scl.y;
        mtx[r][2] *= scl.z;
    }
}''',
    'J3DScaleNrmMtx33': '''void J3DScaleNrmMtx33(Mtx33 mtx, const Vec& scale) {
    for (int r = 0; r < 3; r++) {
        mtx[r][0] *= scale.x;
        mtx[r][1] *= scale.y;
        mtx[r][2] *= scale.z;
    }
}''',
    'J3DMtxProjConcat': '''void J3DMtxProjConcat(Mtx mtx1, Mtx mtx2, Mtx dst) {
    // dst(3x4) = mtx1(3x4) * mtx2, where mtx2 is read as a full 4x4.
    const f32* m2 = &mtx2[0][0];
    Mtx tmp;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            tmp[r][c] = mtx1[r][0] * m2[0 * 4 + c] + mtx1[r][1] * m2[1 * 4 + c] + mtx1[r][2] * m2[2 * 4 + c] + mtx1[r][3] * m2[3 * 4 + c];
        }
    }
    PSMTXCopy(tmp, dst);
}''',
}

text = read(F)
pat = re.compile(r'asm void (J3D\w+)\(([^)]*)\) \{\n#ifdef __MWERKS__  // clang-format off\n(.*?)#endif  // clang-format on\n\}', re.S)

def repl(m):
    name = m.group(1)
    if name not in C_IMPLS:
        return m.group(0)
    return ('#ifdef __MWERKS__\nasm void %s(%s) {\n%s}\n#else\n%s\n#endif' % (name, m.group(2), m.group(3), C_IMPLS[name]))

new, n = pat.subn(repl, text)
if n != 4 and 'J3DMtxProjConcat(Mtx mtx1, Mtx mtx2, Mtx dst) {\n    // dst' not in text:
    print('expected 4 asm functions, got', n)
    sys.exit(1)
write(F, new)

sub(F, '''#undef FP31
#undef UNIT_R
}
#endif  // clang-format on''', '''#undef FP31
#undef UNIT_R
}
#else
void J3DPSMtxArrayConcat(Mtx mA, Mtx mB, Mtx mAB, u32 count) {
    // mB and mAB point at arrays of `count` consecutive 3x4 matrices.
    Mtx* pB = reinterpret_cast< Mtx* >(mB);
    Mtx* pAB = reinterpret_cast< Mtx* >(mAB);
    for (u32 i = 0; i < count; i++) {
        PSMTXConcat(mA, pB[i], pAB[i]);
    }
}
#endif  // clang-format on''')

done()
