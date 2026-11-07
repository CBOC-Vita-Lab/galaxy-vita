// The resampler and AFC decoder dispatch (GXV_DSPRESAMPLE, GXV_DSPAFC) against copies of the original loops, then the
// whole mixer over voices of every input kind; the printed digest must not depend on the modes
// (tools/test_dsp_voice_input.sh). ARAM, voices and tables sit below 4 GiB because the mixer takes 32-bit addresses.
#include "../platform/src/audio/dsp_mixer.cpp"
#include <sys/mman.h>
#include <cstdarg>
#include <cstdint>

static unsigned summaries, failures, resampleSummaries, afcSummaries;
static u8* sLow;  // ARAM (first 2 MiB), then voices and tables
constexpr u32 kAram = 2u << 20;
extern "C" u32 port_dsp_varam_base(void) { return (u32)(uintptr_t)sLow; }
extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char* format, ...) {
    ++summaries;
    if (strstr(format, "mismatch:")) ++failures;
    if (strstr(format, "[DSPRESAMPLE]")) ++resampleSummaries;
    if (strstr(format, "[DSPAFC]")) ++afcSummaries;
}
static void Require(bool ok, const char* why) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); abort(); }
}
static uint32_t state = 0x6c8e9cf5u;
static uint32_t Random() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
static s16 Sample() {  // full range, with the extremes common
    switch (Random() % 8) {
    case 0: return -0x8000;
    case 1: return 0x7FFF;
    default: return s16(Random());
    }
}

// The original resampler and AFC block decoder (dsp_mixer.cpp before the kernels).
static void ReferenceResample(Voice* v, const s16* src, s16* dst) {
    u32 ratio = v->mPitch, pos = (u16)v->_60;
    if ((ratio >> 12) >= 4) {
        for (int i = 0; i < N; i++) {
            pos += ratio;
            dst[i] = src[pos >> 12];
        }
    } else {
        for (int i = 0; i < N; i++) {
            const s16* c = &M.resCoeffs[((pos & 0xFFF) >> 6) * 4];
            const s16* in = &src[pos >> 12];
            s64 acc = 0;
            for (int k = 0; k < 4; k++) acc += (s64)2 * c[k] * in[k];
            dst[i] = clamp16((s32)(acc >> 16));
            pos += ratio;
        }
    }
    for (int k = 0; k < 4; k++) v->_78[k] = src[(pos >> 12) + k];
    v->_66 = dst[N - 1];
    v->_60 = (s16)(pos & 0xFFF);
}
static void ReferenceDecodeAfc(Voice* v, s16* dst, u32 blocks) {
    u32 bytes = v->_100;
    const u8* src = aram(v->_70, blocks * bytes);
    v->_70 += blocks * bytes;
    s16* hist = (s16*)v->_B0;
    s32 yn1 = hist[15], yn2 = hist[14];
    for (u32 b = 0; b < blocks; b++) {
        if (!src) {
            for (int i = 0; i < 16; i++) *dst++ = 0;
            continue;
        }
        s32 scale = 1 << ((src[0] >> 4) & 0xF);
        u32 idx = src[0] & 0xF;
        src++;
        s16 nib[16];
        if (bytes == 9) {
            for (int i = 0; i < 16; i += 2) {
                nib[i] = (s16)((s16)((src[0] >> 4) << 12) >> 1);
                nib[i + 1] = (s16)((s16)((src[0] & 0xF) << 12) >> 1);
                src++;
            }
        } else {
            for (int i = 0; i < 16; i += 4) {
                for (int k = 0; k < 4; k++) nib[i + k] = (s16)((s16)(((src[0] >> (6 - 2 * k)) & 3) << 14) >> 1);
                src++;
            }
        }
        for (int i = 0; i < 16; i++) {
            s32 s = (scale * nib[i] + yn1 * M.afcCoeffs[idx * 2] + yn2 * M.afcCoeffs[idx * 2 + 1]) >> 11;
            s = clamp16(s);
            *dst++ = (s16)s;
            yn2 = yn1;
            yn1 = s;
        }
    }
    hist[14] = (s16)yn2;
    hist[15] = (s16)yn1;
}

static void FillTable(s16* table, int count, int kind) {
    for (int i = 0; i < count; i++) table[i] = kind == 0 ? -0x8000 : kind == 1 ? 0x7FFF : kind == 2 ? s16((i & 1) ? -0x8000 : 0x7FFF) : Sample();
}
static void RandomVoice(Voice* v) {
    u8* bytes = (u8*)v;
    for (size_t j = 0; j < sizeof *v; j++) bytes[j] = u8(Random());
}

static unsigned CheckResample() {
    static s16 src[4 + 0x500 + 0x20];
    unsigned cases = 0;
    static const u16 kPitches[] = {0, 1, 0x0800, 0x0FFF, 0x1000, 0x1001, 0x2000, 0x3FFF, 0x4000, 0xFFFF};
    for (int run = 0; run < 20000; run++) {
        FillTable(M.resCoeffs, 0x100, run < 400 ? run % 4 : 3);
        const bool extreme = run % 16 == 0;
        for (s16& s : src) s = extreme ? s16((run & 32) ? -0x8000 : 0x7FFF) : Sample();
        Voice actual, expected;
        RandomVoice(&actual);
        actual.mPitch = run % 3 ? u16(Random() % 0x4000) : kPitches[Random() % (sizeof kPitches / sizeof *kPitches)];
        expected = actual;
        s16 out[N], ref[N];
        resample(&actual, src, out);
        ReferenceResample(&expected, src, ref);
        Require(!memcmp(out, ref, sizeof out), "resampled samples are exact");
        Require(!memcmp(&actual, &expected, sizeof actual), "position, tail samples and every other voice byte are exact");
        cases++;
    }
    return cases;
}

static unsigned CheckAfc() {
    unsigned cases = 0;
    for (u32 i = 0; i < kAram; i++) sLow[i] = u8(Random());
    for (int run = 0; run < 20000; run++) {
        FillTable(M.afcCoeffs, 0x20, run < 400 ? run % 4 : 3);
        Voice actual, expected;
        RandomVoice(&actual);
        actual._100 = Random() & 1 ? 9 : 5;
        const u32 blocks = run % 5 == 0 ? 1 : Random() % 0x53;
        actual._70 = run % 97 == 0 ? 0x2000000 + Random() % 64 : Random() % (kAram - 0x52 * 9);  // (some unmapped: silence)
        if (run % 7 == 0) actual._B0[14] = actual._B0[15] = (run & 8) ? 0x8000 : 0x7FFF;
        expected = actual;
        const bool intoCache = run % 5 == 0;  // the loop refill decodes one block into the voice's cache
        s16 out[0x520], ref[0x520];
        decodeAfc(&actual, intoCache ? (s16*)actual._B0 : out, blocks);
        ReferenceDecodeAfc(&expected, intoCache ? (s16*)expected._B0 : ref, blocks);
        if (!intoCache) Require(!memcmp(out, ref, blocks * 32), "decoded samples are exact");
        Require(!memcmp(&actual, &expected, sizeof actual), "history, ARAM address and every other voice byte are exact");
        cases++;
    }
    return cases;
}

// 24 voices of every input kind (AFC 4-/2-bit, PCM8, PCM16, oscillators), looping and one-shot, pitched above and below
// the 4-tap limit, with filters and both mixing kinds, re-pitched and restarted while mixing; a digest of the output.
static uint64_t MixDigest() {
    const u32 voicesAt = kAram, tableAt = kAram + 0x10000, afcAt = tableAt + 0x500;
    Voice* voices = (Voice*)(sLow + voicesAt);
    u32* table = (u32*)(sLow + tableAt);
    for (int i = 0; i < 0x140; i++) table[i] = Random();
    for (int i = 0; i < 0x40; i++) sLow[afcAt + i] = u8(Random());
    const u32 count = 24;
    static const u16 kSources[] = {9, 5, 9, 5, 8, 16, 9, 16, 0, 1, 3, 4, 7, 10, 11, 12};
    for (u32 i = 0; i < count; i++) {
        Voice* v = &voices[i];
        memset(v, 0, sizeof *v);
        v->mIsActive = 1;
        v->_8 = 1;
        v->_100 = kSources[i % 16];
        v->mPitch = u16(i % 5 == 4 ? 0x4000 + Random() % 0x8000 : Random() % 0x4000);
        v->_118 = Random() % (kAram / 2);
        v->_114 = 0x800 + Random() % 0x8000;  // samples
        v->_110 = Random() % v->_114;
        v->_102 = i % 3 != 0;
        v->_104 = Sample();
        v->_106 = Sample();
        v->_58 = i % 2;
        v->_50 = u16(Random());
        v->_52 = u16(Random() & 0x7FFF);
        v->_54 = u16(Random() & 0x7FFF);
        v->_56 = u16(Random() & 0x7FFF);
        u16* ch = words(v) + W_CHANNELS;
        static const u16 kBuffers[] = {0x0D00, 0x0D60, 0x0F40, 0x0CA0, 0x0E80, 0x0EE0};
        for (int c = 0; c < 6; c++) {
            ch[c * 4] = kBuffers[c];
            ch[c * 4 + 1] = u16(Random() & 0x7FFF);
            ch[c * 4 + 2] = u16(Random() & 0x7FFF);
        }
        if (i % 4 == 1) v->iir_filter_params[4] = u16(Random());
        if (i % 6 == 2) {
            v->mFilterMode = 0x20 | (Random() % 24);
            for (int k = 0; k < 8; k++) v->fir_filter_params[k] = Sample();
            for (int k = 0; k < 4; k++) v->iir_filter_params[k] = s16(Random() % 0x4000);
        }
    }
    port_dsp_setup(count, (u32)(uintptr_t)voices, (u32)(uintptr_t)table, (u32)(uintptr_t)(sLow + afcAt), 0);
    uint64_t digest = 0xcbf29ce484222325ull;
    auto mixIn = [&](const void* p, size_t n) {
        for (size_t j = 0; j < n; j++) digest = (digest ^ ((const u8*)p)[j]) * 0x100000001b3ull;
    };
    s16 outL[N * 7], outR[N * 7];
    for (int frame = 0; frame < 600; frame++) {
        if (frame % 50 == 25) {  // the game re-pitches and restarts voices between frames
            Voice* v = &voices[Random() % count];
            v->mPitch = u16(Random() % 0x4000);
            if (Random() & 1) { v->_8 = 1; v->mIsFinished = 0; }
        }
        port_dsp_mix_frame(7, outL, outR, 0x8000);
        mixIn(outL, sizeof outL);
        mixIn(outR, sizeof outR);
    }
    mixIn(voices, count * sizeof(Voice));
    return digest;
}

int main() {
    sLow = (u8*)mmap((void*)0x10000000, kAram + 0x20000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    Require(sLow != MAP_FAILED && (uintptr_t)sLow + kAram + 0x20000 <= 0xFFFFFFFFu, "low mapping for 32-bit mixer addresses");
    const int resampleMode = port::dspresample::Mode(), afcMode = port::dspafc::Mode();
    const unsigned resampleCases = CheckResample(), afcCases = CheckAfc();
    const uint64_t digest = MixDigest();
    Require(failures == 0, "the verify modes found no mismatches");
    Require((resampleSummaries > 0) == (resampleMode == 2) && (afcSummaries > 0) == (afcMode == 2) &&
                summaries == resampleSummaries + afcSummaries,
            "each verify mode reports to the basic log; the others stay quiet");
    setenv("GXV_DSPRESAMPLE", resampleMode == 2 ? "0" : "2", 1);
    setenv("GXV_DSPAFC", afcMode == 2 ? "0" : "2", 1);
    Require(port::dspresample::Mode() == resampleMode && port::dspafc::Mode() == afcMode, "the switches are read once");
    printf("digest %016llx resample=%d afc=%d neon=%d: %u resampler and %u AFC cases exact, summaries=%u\n",
           (unsigned long long)digest, resampleMode, afcMode, int(port::dspresample::kHasNeon), resampleCases, afcCases, summaries);
}
