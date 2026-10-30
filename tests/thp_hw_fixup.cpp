// Host test of the THP -> standard JPEG stream fix-up (thpToStandardJpeg in platform/src/sdk/thp_video.cpp), which feeds the
// Vita's hardware JPEG decoder (GXV_THPHW).  No game data: the frames are made here from synthetic pictures with libjpeg,
// then turned into the form THP stores them in (byte stuffing and restart markers removed, see thpStyle).
//
// Checks, for several sizes, qualities and restart intervals:
//   1. fix-up(THP frame) is byte for byte the libjpeg stream the frame came from (also with the end marker missing, with
//      zero padding after it, and with a 64-byte slack of garbage-free zeros the way the pipeline holds frames);
//   2. libjpeg decodes the fixed-up stream, and its planes match the planes the game's own THP decoder makes from the same
//      THP frame within kMaxDiff (2) levels (the two use different IDCTs: a float AAN one here and libjpeg's integer one);
//   3. frames the fix-up must refuse (no tables, 4:4:4, truncated, too little room) are refused.
#define THP_STANDALONE 1
#include "../platform/src/sdk/thp_video.cpp"

#include <jpeglib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

extern "C" void port_gpu_memory_written(void*, uint32_t) {}

static int gFailures = 0;
#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            gFailures++;                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                 \
            printf("\n");                        \
        }                                        \
    } while (0)

static const int kMaxDiff = 2;  // largest accepted level difference between the two decoders' planes

struct Planes {
    int w = 0, h = 0, cw = 0, ch = 0;
    std::vector<u8> y, u, v;  // raster, padded to whole 16-pixel rows and columns (stride w16 / cw16)
    int w16 = 0, h16 = 0;
};

static Planes makePicture(int w, int h, unsigned seed) {
    Planes p;
    p.w = w;
    p.h = h;
    p.cw = (w + 1) / 2;
    p.ch = (h + 1) / 2;
    p.w16 = (w + 15) & ~15;
    p.h16 = (h + 15) & ~15;
    p.y.assign((size_t)p.w16 * p.h16, 0);
    p.u.assign((size_t)p.w16 / 2 * p.h16 / 2, 128);
    p.v.assign((size_t)p.w16 / 2 * p.h16 / 2, 128);
    srand(seed);
    for (int y = 0; y < p.h16; y++) {
        for (int x = 0; x < p.w16; x++) {
            const int xx = x < w ? x : w - 1, yy = y < h ? y : h - 1;  // replicate the edge into the padding
            double v = 128 + 90 * sin(xx * 0.05) * cos(yy * 0.07) + ((xx / 16 + yy / 16) % 2 ? 40 : -40);
            if ((xx * 7 + yy * 13) % 97 < 6) v = 255;  // saturated specks: 0xFF in the stream
            v += (rand() % 41) - 20;                   // noise: long AC codes
            p.y[(size_t)y * p.w16 + x] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
    for (int y = 0; y < p.h16 / 2; y++) {
        for (int x = 0; x < p.w16 / 2; x++) {
            const int xx = x < p.cw ? x : p.cw - 1, yy = y < p.ch ? y : p.ch - 1;
            double a = 128 + 70 * sin(xx * 0.09 + yy * 0.03) + (rand() % 21) - 10;
            double b = 128 + 70 * cos(xx * 0.02 - yy * 0.08) + (rand() % 21) - 10;
            p.u[(size_t)y * (p.w16 / 2) + x] = (u8)(a < 0 ? 0 : a > 255 ? 255 : a);
            p.v[(size_t)y * (p.w16 / 2) + x] = (u8)(b < 0 ? 0 : b > 255 ? 255 : b);
        }
    }
    return p;
}

// Baseline 4:2:0 JPEG, no JFIF header, optimal Huffman tables, the given restart interval (in MCUs).
static std::vector<u8> encode(const Planes& p, int quality, int restartInterval, bool sampling444 = false) {
    jpeg_compress_struct c;
    jpeg_error_mgr err;
    c.err = jpeg_std_error(&err);
    jpeg_create_compress(&c);
    unsigned char* mem = nullptr;
    unsigned long memSize = 0;
    jpeg_mem_dest(&c, &mem, &memSize);
    c.image_width = p.w;
    c.image_height = p.h;
    c.input_components = 3;
    c.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&c);
    jpeg_set_colorspace(&c, JCS_YCbCr);
    c.write_JFIF_header = FALSE;
    c.write_Adobe_marker = FALSE;
    c.comp_info[0].h_samp_factor = sampling444 ? 1 : 2;
    c.comp_info[0].v_samp_factor = sampling444 ? 1 : 2;
    c.comp_info[1].h_samp_factor = c.comp_info[1].v_samp_factor = 1;
    c.comp_info[2].h_samp_factor = c.comp_info[2].v_samp_factor = 1;
    c.raw_data_in = TRUE;
    c.optimize_coding = TRUE;
    c.restart_interval = restartInterval;
    jpeg_set_quality(&c, quality, TRUE);
    jpeg_start_compress(&c, TRUE);
    std::vector<u8> uu(p.u), vv(p.v);
    JSAMPROW yRows[16], uRows[8], vRows[8];
    JSAMPARRAY planes[3] = {yRows, uRows, vRows};
    if (sampling444) {
        // (only for the refusal test: the planes are not meant to look right)
        std::vector<u8> full((size_t)p.w16 * p.h16, 128);
        JSAMPROW rows[8], urows[8], vrows[8];
        JSAMPARRAY pl[3] = {rows, urows, vrows};
        for (int row = 0; row < p.h16; row += 8) {
            for (int i = 0; i < 8; i++) {
                rows[i] = (JSAMPROW)(p.y.data() + (size_t)(row + i) * p.w16);
                urows[i] = vrows[i] = (JSAMPROW)(full.data() + (size_t)(row + i) * p.w16);
            }
            jpeg_write_raw_data(&c, pl, 8);
        }
    } else {
        for (int row = 0; row < p.h16; row += 16) {
            for (int i = 0; i < 16; i++) yRows[i] = (JSAMPROW)(p.y.data() + (size_t)(row + i) * p.w16);
            for (int i = 0; i < 8; i++) {
                uRows[i] = (JSAMPROW)(uu.data() + (size_t)(row / 2 + i) * (p.w16 / 2));
                vRows[i] = (JSAMPROW)(vv.data() + (size_t)(row / 2 + i) * (p.w16 / 2));
            }
            jpeg_write_raw_data(&c, planes, 16);
        }
    }
    jpeg_finish_compress(&c);
    std::vector<u8> out(mem, mem + memSize);
    free(mem);
    jpeg_destroy_compress(&c);
    return out;
}

// What THP stores: the headers as they are, the scan without 0xFF00 stuffing and without RSTn markers, the end marker kept.
static std::vector<u8> thpStyle(const std::vector<u8>& jpg) {
    std::vector<u8> out;
    size_t i = 0;
    bool inScan = false;
    const size_t n = jpg.size();
    while (i < n) {
        if (!inScan) {
            if (jpg[i] == 0xFF && i + 1 < n && jpg[i + 1] != 0 && jpg[i + 1] != 0xFF) {
                const u8 m = jpg[i + 1];
                if (m == 0xD8 || m == 0xD9) {
                    out.push_back(0xFF);
                    out.push_back(m);
                    i += 2;
                    continue;
                }
                const size_t len = (size_t)jpg[i + 2] << 8 | jpg[i + 3];
                out.insert(out.end(), jpg.begin() + i, jpg.begin() + i + 2 + len);
                i += 2 + len;
                if (m == 0xDA) inScan = true;
                continue;
            }
            out.push_back(jpg[i++]);
        } else {
            if (jpg[i] == 0xFF && i + 1 < n) {
                if (jpg[i + 1] == 0x00) {
                    out.push_back(0xFF);
                    i += 2;
                    continue;
                }
                if (jpg[i + 1] >= 0xD0 && jpg[i + 1] <= 0xD7) {
                    i += 2;  // restart marker: THP only realigns
                    continue;
                }
                if (jpg[i + 1] == 0xD9) {
                    out.push_back(0xFF);
                    out.push_back(0xD9);
                    i += 2;
                    inScan = false;
                    continue;
                }
            }
            out.push_back(jpg[i++]);
        }
    }
    return out;
}

// libjpeg decode to planes (raw data, no upsampling, integer IDCT).  false: libjpeg refused the stream.
static bool jpegDecode(const std::vector<u8>& jpg, Planes& p) {
    jpeg_decompress_struct c;
    jpeg_error_mgr err;
    c.err = jpeg_std_error(&err);
    err.error_exit = [](j_common_ptr) { throw 1; };
    err.output_message = [](j_common_ptr) {};
    jpeg_create_decompress(&c);
    bool ok = false;
    try {
        jpeg_mem_src(&c, jpg.data(), jpg.size());
        jpeg_read_header(&c, TRUE);
        c.raw_data_out = TRUE;
        c.do_fancy_upsampling = FALSE;
        c.dct_method = JDCT_ISLOW;
        c.out_color_space = JCS_YCbCr;
        jpeg_start_decompress(&c);
        p.w = c.output_width;
        p.h = c.output_height;
        p.cw = (p.w + 1) / 2;
        p.ch = (p.h + 1) / 2;
        p.w16 = (p.w + 15) & ~15;
        p.h16 = (p.h + 15) & ~15;
        p.y.assign((size_t)p.w16 * p.h16, 0);
        p.u.assign((size_t)p.w16 / 2 * p.h16 / 2, 0);
        p.v.assign((size_t)p.w16 / 2 * p.h16 / 2, 0);
        JSAMPROW yRows[16], uRows[8], vRows[8];
        JSAMPARRAY planes[3] = {yRows, uRows, vRows};
        while (c.output_scanline < c.output_height) {
            const int row = c.output_scanline;
            for (int i = 0; i < 16; i++) yRows[i] = p.y.data() + (size_t)(row + i) * p.w16;
            for (int i = 0; i < 8; i++) {
                uRows[i] = p.u.data() + (size_t)(row / 2 + i) * (p.w16 / 2);
                vRows[i] = p.v.data() + (size_t)(row / 2 + i) * (p.w16 / 2);
            }
            jpeg_read_raw_data(&c, planes, 16);
        }
        jpeg_finish_decompress(&c);
        ok = true;
    } catch (int) {
    }
    jpeg_destroy_decompress(&c);
    return ok;
}

static u8 tileTexel(const u8* tiles, u32 width, u32 x, u32 y) {
    const u32 tilesPerRow = (width + 7) / 8;
    return tiles[((size_t)(y >> 2) * tilesPerRow + (x >> 3)) * 32 + (y & 3) * 8 + (x & 7)];
}

struct Diff {
    int maxDiff = 0;
    double mean = 0;
};

// The game's decoder (GX tiles) against libjpeg's planes, over the visible picture.
static Diff compareWithGame(const Planes& jp, const std::vector<u8>& ty, const std::vector<u8>& tu, const std::vector<u8>& tv) {
    Diff d;
    double sum = 0;
    size_t n = 0;
    auto cmp = [&](int a, int b) {
        const int e = abs(a - b);
        if (e > d.maxDiff) d.maxDiff = e;
        sum += e;
        n++;
    };
    for (int y = 0; y < jp.h; y++)
        for (int x = 0; x < jp.w; x++) cmp(jp.y[(size_t)y * jp.w16 + x], tileTexel(ty.data(), jp.w, x, y));
    for (int y = 0; y < jp.ch; y++) {
        for (int x = 0; x < jp.cw; x++) {
            cmp(jp.u[(size_t)y * (jp.w16 / 2) + x], tileTexel(tu.data(), jp.cw, x, y));
            cmp(jp.v[(size_t)y * (jp.w16 / 2) + x], tileTexel(tv.data(), jp.cw, x, y));
        }
    }
    d.mean = n ? sum / (double)n : 0;
    return d;
}

// A frame the way the pipeline holds it: the bytes, then 64 zero bytes of slack.
static std::vector<u8> padded(const std::vector<u8>& f) {
    std::vector<u8> v(f);
    v.resize(f.size() + 64, 0);
    return v;
}

static s32 fixup(Decoder& d, const std::vector<u8>& thp, std::vector<u8>& out, u32* w = nullptr, u32* h = nullptr) {
    const std::vector<u8> in = padded(thp);
    out.assign(2 * thp.size() + 1024, 0);
    u32 n = 0, ww = 0, hh = 0;
    const s32 st = thpToStandardJpeg(d, in.data(), (u32)thp.size(), out.data(), (u32)out.size(), &n, &ww, &hh);
    if (st == 0) out.resize(n);
    if (w) *w = ww;
    if (h) *h = hh;
    return st;
}

int main() {
    static Decoder dec;  // one decoder for the whole run, as a worker keeps one: the table caches are exercised too
    memset(&dec, 0, sizeof(dec));
    struct Case {
        int w, h, quality, ri;
    };
    const Case cases[] = {
        {640, 368, 72, 0},  {640, 368, 72, 1},  {640, 368, 30, 40}, {640, 480, 95, 0}, {640, 480, 85, 17},
        {320, 240, 60, 3},  {200, 120, 90, 0},  {200, 120, 90, 5},  {100, 60, 50, 2},  {37, 29, 80, 0},
        {37, 29, 80, 1},    {48, 32, 75, 6},    {64, 16, 99, 1},    {16, 16, 40, 0},   {640, 368, 72, 100000},
    };
    int caseNo = 0;
    int worstOverall = 0;
    double worstMean = 0;
    for (const Case& cs : cases) {
        caseNo++;
        const Planes pic = makePicture(cs.w, cs.h, 1000 + caseNo);
        const std::vector<u8> ref = encode(pic, cs.quality, cs.ri);
        const std::vector<u8> thp = thpStyle(ref);
        char label[96];
        snprintf(label, sizeof label, "%dx%d q%d ri%d", cs.w, cs.h, cs.quality, cs.ri);

        // 1. byte-exact round trip, in the shapes a frame can come in
        std::vector<u8> fixed;
        u32 w = 0, h = 0;
        s32 st = fixup(dec, thp, fixed, &w, &h);
        CHECK(st == 0, "%s: fix-up status %d", label, st);
        CHECK(fixed == ref, "%s: fixed-up stream differs from the libjpeg stream (%zu vs %zu bytes)", label, fixed.size(), ref.size());
        CHECK(w == (u32)cs.w && h == (u32)cs.h, "%s: size %ux%u", label, w, h);

        std::vector<u8> noEoi(thp.begin(), thp.end() - 2);  // no end marker
        st = fixup(dec, noEoi, fixed);
        CHECK(st == 0 && fixed == ref, "%s: without the end marker: status %d, %s", label, st, fixed == ref ? "equal" : "different");

        std::vector<u8> pad(thp);  // end marker, then padding to 32 bytes the way the container does
        while (pad.size() & 31) pad.push_back(0);
        st = fixup(dec, pad, fixed);
        CHECK(st == 0 && fixed == ref, "%s: zero padded: status %d, %s", label, st, fixed == ref ? "equal" : "different");

        // 2. libjpeg decodes it; the game's decoder agrees with libjpeg
        Planes jp;
        CHECK(jpegDecode(fixed, jp), "%s: libjpeg refused the fixed-up stream", label);
        if (jp.w == cs.w) {
            static Decoder sw;
            memset(&sw, 0, sizeof(sw));
            const std::vector<u8> in = padded(thp);
            const u32 lb = lumaBytes(cs.w, cs.h), cb = chromaBytes(cs.w, cs.h);
            std::vector<u8> ty(lb, 0x10), tu(cb, 0x80), tv(cb, 0x80);
            const s32 ds = decodeFrame(sw, in.data(), (u32)thp.size(), ty.data(), tu.data(), tv.data());
            CHECK(ds == 0, "%s: the game's decoder status %d", label, ds);
            const Diff df = compareWithGame(jp, ty, tu, tv);
            printf("%-24s stream %6zu bytes -> %6zu: exact round trip; game decoder vs libjpeg: max %d, mean %.3f\n", label, thp.size(), fixed.size(),
                   df.maxDiff, df.mean);
            CHECK(df.maxDiff <= kMaxDiff, "%s: planes differ by up to %d levels (limit %d)", label, df.maxDiff, kMaxDiff);
            if (df.maxDiff > worstOverall) worstOverall = df.maxDiff;
            if (df.mean > worstMean) worstMean = df.mean;
        }
    }

    // 3. refusals
    {
        const Planes pic = makePicture(64, 32, 7);
        const std::vector<u8> ref = encode(pic, 70, 0);
        const std::vector<u8> thp = thpStyle(ref);
        std::vector<u8> out;
        Decoder d;
        memset(&d, 0, sizeof(d));
        CHECK(fixup(d, thp, out) == 0, "baseline frame refused");

        // no DHT: cut the Huffman segments out
        std::vector<u8> noDht;
        for (size_t i = 0; i < thp.size();) {
            if (thp[i] == 0xFF && i + 1 < thp.size() && thp[i + 1] == 0xC4) {
                i += 2 + ((size_t)thp[i + 2] << 8 | thp[i + 3]);
                continue;
            }
            noDht.push_back(thp[i++]);
            if (noDht.size() > 1 && noDht[noDht.size() - 2] == 0xFF && noDht.back() == 0xDA) {  // copy the rest verbatim
                noDht.insert(noDht.end(), thp.begin() + i, thp.end());
                break;
            }
        }
        // (the decoder `d` has seen the tables before: they must still be refused, a standard decoder has none of its own)
        CHECK(fixup(d, noDht, out) != 0, "a frame without Huffman tables was accepted");

        std::vector<u8> noDqt;
        for (size_t i = 0; i < thp.size();) {
            if (thp[i] == 0xFF && i + 1 < thp.size() && thp[i + 1] == 0xDB) {
                i += 2 + ((size_t)thp[i + 2] << 8 | thp[i + 3]);
                continue;
            }
            noDqt.push_back(thp[i++]);
            if (noDqt.size() > 1 && noDqt[noDqt.size() - 2] == 0xFF && noDqt.back() == 0xDA) {
                noDqt.insert(noDqt.end(), thp.begin() + i, thp.end());
                break;
            }
        }
        CHECK(fixup(d, noDqt, out) != 0, "a frame without quantization tables was accepted");

        const std::vector<u8> t444 = thpStyle(encode(pic, 70, 0, true));
        CHECK(fixup(d, t444, out) != 0, "a 4:4:4 frame was accepted");

        std::vector<u8> cut(thp.begin(), thp.begin() + 20);
        CHECK(fixup(d, cut, out) != 0, "a frame cut inside the headers was accepted");

        // truncated scan with a restart interval: the walk runs out of data
        const std::vector<u8> thpRi = thpStyle(encode(makePicture(64, 32, 8), 70, 1));
        std::vector<u8> cutRi(thpRi.begin(), thpRi.end() - 40);
        CHECK(fixup(d, cutRi, out) != 0, "a truncated scan with restarts was accepted");

        // too little room
        std::vector<u8> small(thp.size() / 2);
        u32 n = 0, w = 0, h = 0;
        const std::vector<u8> in = padded(thp);
        CHECK(thpToStandardJpeg(d, in.data(), (u32)thp.size(), small.data(), (u32)small.size(), &n, &w, &h) != 0, "output overflow went unnoticed");

        CHECK(fixup(d, std::vector<u8>(), out) != 0, "an empty frame was accepted");
        CHECK(fixup(d, std::vector<u8>(100, 0x55), out) != 0, "garbage was accepted");
    }

    printf("worst difference between the game's decoder and libjpeg over all cases: %d levels (mean %.3f)\n", worstOverall, worstMean);
    printf(gFailures ? "FAILED: %d check(s)\n" : "ok\n", gFailures);
    return gFailures ? 1 : 0;
}
