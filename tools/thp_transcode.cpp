// Re-encodes the video of a cooked (little-endian) THP movie at a lower bitrate, keeping the container, the
// frame rate, the audio and the picture size.  The movies the game ships are ~40 KB of JPEG per frame at 59.94
// frames a second (2.4 MB/s); the Vita spends most of the decode on the entropy-coded data, so a smaller
// picture stream plays at full speed and the files shrink to a third.
//
//   thp_transcode in.thp out.thp [quality=72] [max frames] [sharpen strength 0..1]
//
// Frames are decoded with the game's own decoder (platform/src/sdk/thp_video.cpp, THP_STANDALONE), re-encoded
// as baseline 4:2:0 JPEG with per-frame optimal Huffman tables, and written the way THP stores them (the 0xFF00
// byte stuffing removed from the entropy-coded data).
//
//   g++ -O2 -std=c++17 -o thp_transcode tools/thp_transcode.cpp -ljpeg
#define THP_STANDALONE 1
#include "../platform/src/sdk/thp_video.cpp"

#include <jpeglib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

extern "C" void port_gpu_memory_written(void*, uint32_t) {}

static u32 rd32(const u8* p) { return (u32)p[3] << 24 | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0]; }
static void wr32(u8* p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static const int kW = 640, kH = 368;

// The decoder's 8x4 tiles (32 bytes each, tilesPerRow across) back to raster order.
static void detile(const u8* tiled, u8* raster, int w, int h) {
    const int tilesPerRow = w / 8;
    for (int y = 0; y < h; ++y) {
        const u8* row = tiled + (size_t)((y >> 2) * tilesPerRow) * 32 + (y & 3) * 8;
        u8* out = raster + (size_t)y * w;
        for (int tx = 0; tx < tilesPerRow; ++tx) memcpy(out + tx * 8, row + tx * 32, 8);
    }
}

// Removes the 0xFF00 stuffing from the scan (THP does not have it).
static std::vector<u8> thpStyle(const unsigned char* jpg, unsigned long n) {
    std::vector<u8> out;
    out.reserve(n);
    size_t i = 0;
    bool inScan = false;
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
                out.insert(out.end(), jpg + i, jpg + i + 2 + len);
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

static std::vector<u8> encode(const u8* Y, const u8* U, const u8* V, int quality) {
    jpeg_compress_struct c;
    jpeg_error_mgr err;
    c.err = jpeg_std_error(&err);
    jpeg_create_compress(&c);
    unsigned char* mem = nullptr;
    unsigned long memSize = 0;
    jpeg_mem_dest(&c, &mem, &memSize);
    c.image_width = kW;
    c.image_height = kH;
    c.input_components = 3;
    c.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&c);
    jpeg_set_colorspace(&c, JCS_YCbCr);
    c.write_JFIF_header = FALSE;
    c.write_Adobe_marker = FALSE;
    c.comp_info[0].h_samp_factor = 2;
    c.comp_info[0].v_samp_factor = 2;
    c.comp_info[1].h_samp_factor = c.comp_info[1].v_samp_factor = 1;
    c.comp_info[2].h_samp_factor = c.comp_info[2].v_samp_factor = 1;
    c.raw_data_in = TRUE;
    c.optimize_coding = TRUE;
    jpeg_set_quality(&c, quality, TRUE);
    jpeg_start_compress(&c, TRUE);
    JSAMPROW yRows[16], uRows[8], vRows[8];
    JSAMPARRAY planes[3] = {yRows, uRows, vRows};
    for (int row = 0; row < kH; row += 16) {
        for (int i = 0; i < 16; ++i) yRows[i] = (JSAMPROW)(Y + (size_t)(row + i) * kW);
        for (int i = 0; i < 8; ++i) {
            uRows[i] = (JSAMPROW)(U + (size_t)(row / 2 + i) * (kW / 2));
            vRows[i] = (JSAMPROW)(V + (size_t)(row / 2 + i) * (kW / 2));
        }
        jpeg_write_raw_data(&c, planes, 16);
    }
    jpeg_finish_compress(&c);
    std::vector<u8> out = thpStyle(mem, memSize);
    free(mem);
    jpeg_destroy_compress(&c);
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.thp out.thp [quality] [max frames]\n", argv[0]);
        return 2;
    }
    const int quality = argc > 3 ? atoi(argv[3]) : 72;
    const long maxFrames = argc > 4 ? atol(argv[4]) : -1;
    FILE* in = fopen(argv[1], "rb");
    if (!in) { perror("open input"); return 1; }
    u8 head[96];
    if (fread(head, 1, 96, in) != 96) return 1;
    if (memcmp(head, "THP\0", 4)) { fprintf(stderr, "not a THP file\n"); return 1; }
    const u32 numFrames = rd32(head + 0x14), firstSize = rd32(head + 0x18), movieOffset = rd32(head + 0x28);
    const u32 total = maxFrames >= 0 && (u32)maxFrames < numFrames ? (u32)maxFrames : numFrames;
    FILE* out = fopen(argv[2], "wb");
    if (!out) { perror("open output"); return 1; }
    fwrite(head, 1, 96, out);  // fixed up at the end

    const size_t pl = (size_t)kW * kH, pc = pl / 4;
    std::vector<u8> src(128 * 1024), ty(pl), tu(pc), tv(pc), ry(pl), ru(pc), rv(pc), work(4096);
    u64 inBytes = 0, outBytes = 0, written = 96, lastOffset = 96;
    u32 prevOutSize = 0, curSize = firstSize, maxOut = 0, firstOut = 0;
    std::vector<u8> pending;  // the previous output frame, waiting for its "next" field
    u32 pendingSize = 0;
    long frames = 0;
    double sumMse = 0, worstMse = 0;
    u64 pos = movieOffset;
    for (u32 i = 0; i < total; ++i) {
        fseek(in, (long)pos, SEEK_SET);
        if (curSize > src.size() || fread(src.data(), 1, curSize, in) != curSize) { fprintf(stderr, "short read at frame %u\n", i); return 1; }
        const u32 nextSize = rd32(src.data());
        const u32 vsz = rd32(src.data() + 8), asz = rd32(src.data() + 12);
        const u8* video = src.data() + 16;
        const u8* audio = video + vsz;
        memset(ty.data(), 0, pl); memset(tu.data(), 0, pc); memset(tv.data(), 0, pc);
        if (THPVideoDecode((void*)video, ty.data(), tu.data(), tv.data(), work.data())) { fprintf(stderr, "decode error at frame %u\n", i); return 1; }
        detile(ty.data(), ry.data(), kW, kH);
        detile(tu.data(), ru.data(), kW / 2, kH / 2);
        detile(tv.data(), rv.data(), kW / 2, kH / 2);
        std::vector<u8> jpg = encode(ry.data(), ru.data(), rv.data(), quality);
        while (jpg.size() & 3) jpg.push_back(0);
        if (getenv("THP_PSNR")) {  // how far the new picture is from the old, measured on what the game would show
            std::vector<u8> padded(jpg);
            padded.resize(padded.size() + 64, 0);
            std::vector<u8> cy(pl), cu(pc), cv(pc);
            if (THPVideoDecode(padded.data(), cy.data(), cu.data(), cv.data(), work.data())) { fprintf(stderr, "re-decode error at frame %u\n", i); return 1; }
            double se = 0;
            for (size_t k = 0; k < pl; ++k) { double d = (double)cy[k] - ty[k]; se += d * d; }
            sumMse += se / pl;
            if (se / pl > worstMse) worstMse = se / pl;
        }
        const u32 vOut = (u32)jpg.size();
        u32 frameSize = 16 + vOut + asz;
        frameSize = (frameSize + 31) & ~31u;
        std::vector<u8> fr(frameSize, 0);
        wr32(fr.data() + 4, prevOutSize);
        wr32(fr.data() + 8, vOut);
        wr32(fr.data() + 12, asz);
        memcpy(fr.data() + 16, jpg.data(), vOut);
        memcpy(fr.data() + 16 + vOut, audio, asz);
        // The previous frame learns the size of this one.
        if (!pending.empty()) {
            wr32(pending.data(), frameSize);
            fwrite(pending.data(), 1, pending.size(), out);
            written += pending.size();
        } else {
            firstOut = frameSize;
        }
        pending.swap(fr);
        pendingSize = frameSize;
        prevOutSize = frameSize;
        if (frameSize > maxOut) maxOut = frameSize;
        inBytes += curSize;
        outBytes += frameSize;
        ++frames;
        pos += curSize;
        curSize = nextSize;
        if (frames % 500 == 0) fprintf(stderr, "%ld frames, %.1f -> %.1f KB/frame\n", frames, inBytes / 1024.0 / frames, outBytes / 1024.0 / frames);
    }
    // The last frame points at the first (THP loops), as the source does.
    wr32(pending.data(), firstOut);
    lastOffset = written;
    fwrite(pending.data(), 1, pending.size(), out);
    written += pending.size();
    // First frame's "previous" is the last one.
    // Header fixes.
    wr32(head + 0x14, (u32)frames);
    wr32(head + 0x18, firstOut);
    wr32(head + 0x1c, (u32)written);  // (the file's size, as in the source)
    wr32(head + 0x2c, (u32)lastOffset);
    wr32(head + 8, (maxOut + 31) & ~31u);
    fseek(out, 0, SEEK_SET);
    fwrite(head, 1, 96, out);
    // First frame's prev field = last frame size.
    fseek(out, 96 + 4, SEEK_SET);
    u8 pv[4];
    wr32(pv, prevOutSize);
    fwrite(pv, 1, 4, out);
    fclose(out);
    if (getenv("THP_PSNR") && frames)
        fprintf(stderr, "luma PSNR: mean-MSE %.1f dB, worst frame %.1f dB\n", 10 * log10(255.0 * 255.0 / (sumMse / frames + 1e-9)),
                10 * log10(255.0 * 255.0 / (worstMse + 1e-9)));
    fprintf(stderr, "done: %ld frames, %.2f MB -> %.2f MB (%.1f -> %.1f KB per frame), max frame %u\n", frames, inBytes / 1048576.0,
            outBytes / 1048576.0, inBytes / 1024.0 / frames, outBytes / 1024.0 / frames, maxOut);
    return 0;
}
