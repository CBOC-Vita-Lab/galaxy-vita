// HD texture replacement: a pack of higher-resolution, GPU-ready (DXT1/DXT5, with their own
// mip chains) versions of selected guest textures, keyed by a hash of the original texels.
// The texture cache (vgx_texcache.cpp) asks once per upload of a guest texture; nothing is
// done per frame or per bind. Pack format and rebuild command: tools/hdtex/README.md.
#pragma once
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
//   GXV_HDTEX_PRELOAD=<MiB> read the whole pack into memory at start-up when it is no larger
//                          (default 8: the 4.5 MiB pack is preloaded; 0: read each texture from
//                          the card when it is first uploaded)
//   GXV_HDTEX_TRILINEAR=1  sample replaced textures with GL_LINEAR_MIPMAP_LINEAR (default 0:
//                          GL_LINEAR_MIPMAP_NEAREST, the cost of the original bilinear fetch)
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
};
void GetCounters(Counters& c);

} // namespace vgx::hdtex
