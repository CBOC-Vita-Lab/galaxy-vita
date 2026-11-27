// HD texture replacement: a pack of higher-resolution, GPU-ready (DXT1/DXT5, with their own
// mip chains) versions of selected guest textures, keyed by a hash of the original texels.
// The texture cache (vgx_texcache.cpp) asks once per upload of a guest texture; nothing is
// done per frame or per bind. Pack format and rebuild command: tools/hdtex/README.md.
#pragma once
#include <cstddef>
#include <cstdint>

namespace vgx::hdtex {

// Opens the pack and loads its index (the host calls this once at start-up, before the first
// frame; `log` is the host's line logger, may be null). Reads the switches:
//   GXV_HDTEX=0            off (the guest textures as they are)
//   GXV_HDTEX=1            on when the pack exists (default)
//   GXV_HDTEX=2            dry run: look every texture up and count the hits, upload the originals
//                          (GXV_HDTEX=0 with GXV_CHARAA on: the index is still read, to tell the characters' and key
//                          items' textures (vgx::IsCharaTexture); the originals are uploaded)
//   GXV_HDTEX_PATH=<file>  the pack (default ux0:data/smg/hdtex.pack on the Vita, ./hdtex.pack elsewhere)
//   GXV_HDTEX_BUDGET=<MiB> most HD texture bytes alive in the back end at once; past it the
//                          original is used (default 16)
//   GXV_HDTEX_PRELOAD=<MiB> keep this much of the pack's start in memory (default 8: a pack up to that size is read whole at start-up, so
//                          no texture's first use in play, and none re-uploaded after the cache freed it, waits on the card; a larger one
//                          has its first 8 MiB in memory, the builder writes Mario's and Luigi's textures first, the rest is read from
//                          the card per upload; 0: read each texture from the card when it is uploaded). GXV_HDTEX_PRELOAD_KB=<KiB>
//                          replaces it when set (finer; the host test uses it)
//   GXV_HDTEX_TRILINEAR=1  sample replaced textures with GL_LINEAR_MIPMAP_LINEAR (default 0:
//                          GL_LINEAR_MIPMAP_NEAREST, the cost of the original bilinear fetch). Only the filter of the replaced
//                          textures changes (the textures of Mario, the main characters and the key items; a nearest-filtered
//                          original stays nearest): two bilinear fetches instead of one for those pixels, and no step where
//                          the level in use changes. Also the textures anisotropic filtering applies to (GXV_AUTOQ_ANISO:
//                          vitaGL's switch acts on trilinear textures only).
//   GXV_HDTEX_EYES=1       use the pack's eye and eyelid textures too (default 0 here: the game's own; the Vita build sets 1, vita/vgx-game/entry.cpp)
//   GXV_HDTEX_LOG=<n>      log the first n replacements by name (default 48)
void Install(void (*log)(const char* fmt, ...));

struct Counters {
    uint64_t hashed = 0;    // uploads whose format and size occur in the pack (their texels were hashed)
    uint64_t hits = 0;      // replaced (or, GXV_HDTEX=2, would have been)
    uint64_t refused = 0;   // found but over GXV_HDTEX_BUDGET
    uint64_t bytesRead = 0; // pack bytes read for the hits
    uint64_t lookupNs = 0;  // hashing + index search of the hashed uploads
    uint64_t readNs = 0, readMaxNs = 0; // reading the hits' data
    uint64_t peakLive = 0;  // most HD bytes alive at once
    uint32_t entries = 0;   // textures in the pack (0: no pack / off)
    uint64_t cardReads = 0, cardBytes = 0; // reads that went to the file (the memory card) after start-up, not to the preloaded start of the pack
    uint32_t preloaded = 0, fileSize = 0;  // bytes of the pack held in memory / the pack's size
    uint64_t gpuAllAlive = 0;              // GPU bytes if every record were alive (levels down to 4x4, as the console takes them)
};
void GetCounters(Counters& c);

// One basic-log line (log.txt) about what the pack did since the previous call: textures substituted (bytes, how many in play), freed again,
// alive, card reads. The host calls it when a scene is destroyed. False (nothing written) without a pack or when nothing happened.
bool SceneLine(char* buf, size_t n);

} // namespace vgx::hdtex
