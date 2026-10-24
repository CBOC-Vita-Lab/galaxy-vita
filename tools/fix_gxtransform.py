import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch import read, write, done

F = 'src/RVL_SDK/gx/GXTransform.c'
t = read(F)

def wrap(text, start_marker, end_marker, replacement):
    a = text.index(start_marker)
    b = text.index(end_marker, a)
    seg = text[a:b]
    if '#ifdef __MWERKS__' in seg:
        return text
    return text[:a] + '#ifdef __MWERKS__\n' + seg + '#else\n' + replacement + '#endif\n\n' + text[b:]

t = wrap(t, 'static void WriteProjPS(', 'void __GXSetProjection(void) {', '''static void WriteProjPS(const f32 proj[6], volatile void* dest) {
    int i;
    (void)dest;
    for (i = 0; i < 6; i++) {
        GX_WRITE_F32(proj[i]);
    }
}

static void Copy6Floats(const f32 src[6], f32 dst[6]) {
    int i;
    for (i = 0; i < 6; i++) {
        dst[i] = src[i];
    }
}
''')

t = wrap(t, 'static void  WriteMTXPS3x3from3x4(', 'void GXLoadPosMtxImm(', '''static void WriteMTXPS3x3from3x4(void* mtx, volatile void* dest) {
    const f32(*m)[4] = (const f32(*)[4])mtx;
    int r;
    (void)dest;
    for (r = 0; r < 3; r++) {
        GX_WRITE_F32(m[r][0]);
        GX_WRITE_F32(m[r][1]);
        GX_WRITE_F32(m[r][2]);
    }
}

static void WriteMTXPS4x3(const f32 src[3][4], volatile void* dst) {
    int r, c;
    (void)dst;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 4; c++) {
            GX_WRITE_F32(src[r][c]);
        }
    }
}

static void WriteMTXPS4x2(const f32 mtx[3][4], volatile void* dest) {
    int r, c;
    (void)dest;
    for (r = 0; r < 2; r++) {
        for (c = 0; c < 4; c++) {
            GX_WRITE_F32(mtx[r][c]);
        }
    }
}
''')
write(F, t)
done()
