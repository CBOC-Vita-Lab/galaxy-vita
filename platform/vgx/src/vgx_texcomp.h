// GXV_TEX_COMPRESS (texcomp23, 8 Nov 2026): large uncompressed colour textures converted to DXT1 / DXT5 at load, off the
// game's threads, cached on the card. A MINOR CHANGE (lossy, gated per texture by a quality floor); see vgx_texcomp.cpp for the whole
// design, the switches and the log lines.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vgx {
struct CmdBuf;
struct DrawRec;
namespace texcomp {

// ---------------------------------------------------------------- the encoder (pure functions: also the host tests and the eval tool)
// One 4x4 block (16 RGBA texels, row-major). The colour half is always written in DXT1's four-colour mode (colour 0 > colour 1, or both
// equal with every index 0), so that it decodes the same as a DXT1 block and as the colour half of a DXT5 block on any decoder.
void EncodeColorBlock(const uint8_t rgba[64], uint8_t out[8]);
// DXT5's alpha half: the 8-value mode (a0 > a1) or the 6-value mode with explicit 0 and 255, whichever is closer. Texels that are 0 or
// 255 stay exactly 0 or 255 (binary alpha is lossless).
void EncodeAlphaBlock(const uint8_t rgba[64], uint8_t out[8]);
// Whole levels (row-major blocks, as GL_COMPRESSED_RGBA_S3TC_DXT1/5_EXT take them). Edge blocks repeat the last row / column.
void EncodeDxt1(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* dst);
void EncodeDxt5(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* dst);
// The reference decode (exact integer formulas of the S3TC spec, colour 2 = (2 c0 + c1) / 3 rounded), for the quality gate and the tests.
void DecodeDxt(const uint8_t* src, uint32_t w, uint32_t h, bool dxt5, uint8_t* rgba);

struct Quality {
    double psnrRgb = 99.0, psnrA = 99.0; // over the level (99: identical)
    uint32_t maxErrRgb = 0, maxErrA = 0; // largest error of one channel of one texel
};
Quality Measure(const uint8_t* a, const uint8_t* b, size_t texels);

enum : uint8_t { kFmtNone = 0, kFmtDxt1 = 1, kFmtDxt5 = 3 }; // (= vgx_cmd.h TF_DXT1 / TF_DXT5)
struct Encoded {
    uint8_t format = kFmtNone;
    std::vector<std::vector<uint8_t>> levels; // level 0 first
    Quality q;                                // level 0 against the source
    uint8_t lo[4] = {0, 0, 0, 0}, hi[4] = {255, 255, 255, 255}; // per channel range of the decoded levels (pix23's TexChannelRange)
};
// Level 0 = rgba (w x h, powers of two); with `mips`, 2x2 box levels (vgx_texdecode Downsample) while both sides stay >= 4. DXT1 when every
// alpha is 255, else DXT5. Returns false for sizes it does not take (not a power of two, a side under 4).
bool EncodeTexture(const uint8_t* rgba, uint32_t w, uint32_t h, bool mips, Encoded& out);

// ---------------------------------------------------------------- the switch
// GXV_TEX_COMPRESS: 0 (default) off; 1 on in every mode; "perf" on in Performance mode only (GXV_AUTOQ_TARGET=60 without the 30 cap).
// Read once.
int Mode();

// ---------------------------------------------------------------- the front end (vgx_texcache.cpp; front-end thread)
// What the texture cache needs to find its entry again (its Key's fields).
struct EntryRef {
    const void* data = nullptr;
    uint32_t dims = 0, fmt = 0;
    uint64_t tlutHash = 0;
};
// Why a texture was or was not offered (the census and the log count these).
enum Class : uint8_t {
    kClsNone = 0,     // not looked at (switch off, or not an RGBA8-path upload)
    kClsQueued,       // eligible: queued for the worker (or a card-cache hit)
    kClsIA,           // I4 / I8 / IA4 / IA8 (they have the exact LA88 / LA88_MIPS layouts) unless GXV_TEX_COMPRESS_IA=1
    kClsSmall,        // under GXV_TEX_COMPRESS_MIN texels, or a side under 8
    kClsNpot,         // not a power of two (the console's DXT path takes powers of two only)
    kClsPatched,      // the host patches its texels (button pictures), or an HD replacement
    kClsChain,        // its own level chain (the authored I4 chain; desktop chains)
    kClsDynamic,      // its contents changed after the first upload (animated in memory)
    kClsCap,          // first sight, no card-cache entry, and the worker already holds GXV_TEX_COMPRESS_INFLIGHT_MB of texels
    kClsRejected,     // the card cache says it failed the quality floor before
    kClsDxt,          // already compressed (CMPR as DXT1 / DXT5)
    kClsLa88,         // uploaded as LA88
    kClsCount
};
struct OfferIn {
    uint32_t texId = 0;
    EntryRef ref;
    uint64_t contentHash = 0;
    uint32_t gxFmt = 0;
    uint16_t w = 0, h = 0;
    bool mips = false;          // the upload has generated levels (genMips)
    bool patched = false;       // host patch hook / HD
    bool ownChain = false;      // levels other than level 0 are the front end's (not generated)
    bool reuploaded = false;    // not the entry's first upload
    const uint8_t* rgba = nullptr; // level 0, RGBA8 (w x h)
    const uint8_t* la88 = nullptr; // or level 0 as LA88 pairs (an I/IA texture on the LA88 path; GXV_TEX_COMPRESS_IA)
    const uint8_t* i8 = nullptr;   // or level 0 as 1-byte I8 (an I4/I8 texture on the TF_I8 path; GXV_LA88=2)
};
// Which class the texture falls in by itself (no queue, no usage marks): the census's view with the switch off.
Class Classify(const OfferIn& in);
// Decides and, when eligible, queues the texture (a copy of level 0 unless the card cache has it already). Returns the class.
Class Offer(const OfferIn& in);
// A finished conversion waiting to replace its texture.
struct Result {
    uint32_t texId = 0;
    EntryRef ref;
    uint64_t contentHash = 0;
    uint16_t w = 0, h = 0;
    uint8_t format = kFmtNone;
    std::vector<std::vector<uint8_t>> levels;
    uint8_t lo[4] = {0, 0, 0, 0}, hi[4] = {255, 255, 255, 255};
    uint32_t oldBytes = 0;      // what the uncompressed texture holds (filled by the front end when it applies the result)
    bool fromCache = false;
};
// What the front end did with a result.
enum Verdict : uint8_t { kKeep = 0, kApplied, kStale, kDrop2D, kDropInd, kDropRange };
// Front-end frame end: takes the worker's finished results and offers each waiting one to `fn` (at most a few a frame); kKeep leaves it
// waiting (not drawn in 3D yet), anything else removes it. Also the "tex-compress:" log line at most every 30 s.
void FrameEnd(Verdict (*fn)(Result& r, void* ctx), void* ctx);
// The back end's usage marks of a texture id: drawn with a perspective projection, with an orthographic one (2D: layouts, fonts, HUD,
// screen effects), as an indirect map (any of its draws).
enum : uint8_t { kUse3D = 1, kUse2D = 2, kUseInd = 4 };
uint8_t Usage(uint32_t texId);
void UsageReset(uint32_t texId); // (a new upload of the id)
void NoteApplied(uint32_t oldBytes, uint32_t newBytes);

// ---------------------------------------------------------------- the back end (vgx_backend_gl.cpp; render thread; weak there)
// Bit 0: mark usage (the switch is on); bit 1: the texture-read census (GXV_TEXCOMP_CENSUS=1). Read once.
int BackendHooks();
void NoteUse(uint32_t texId, uint8_t use);
// GXV_TEXCOMP_CENSUS=1: per draw, the pixels it covers (a stream draw's triangles, or its rectangle) for each texture it samples, by id;
// render targets (EFB copies, passes) with their size. Every 5 s one "tex-census:" line (the estimated texture bytes read a picture by
// class and what the switch would save) and "tex-census top:" lines (the textures that read the most).
void CensusDraw(uint32_t texId, float px, bool bounded, uint8_t use, bool renderTarget, uint16_t rtW, uint16_t rtH);
void CensusPicture();
// (front end, at every upload while the census is on) what an id holds: GX format, size, upload format and bytes, class.
void CensusInfo(uint32_t texId, uint32_t gxFmt, uint16_t w, uint16_t h, uint8_t upFormat, uint32_t bytes, bool mips, uint8_t cls);
bool CensusOn();

// ---------------------------------------------------------------- tests
struct Counters {
    uint64_t offered = 0, queued = 0, appliedFromCache = 0, hits = 0, encoded = 0, rejected = 0, applied = 0, stale = 0, drop2D = 0, dropInd = 0, dropRange = 0;
    uint64_t bytesBefore = 0, bytesAfter = 0, encodeUs = 0, readUs = 0, writeUs = 0, cls[kClsCount] = {};
    uint64_t waiting = 0, inflightBytes = 0;
};
Counters GetCounters();
void TestDrain(); // waits until the worker has nothing queued (tests)
void TestReset(); // stops nothing; forgets counters, usage marks and the in-memory cache index (tests)
} // namespace texcomp
} // namespace vgx
