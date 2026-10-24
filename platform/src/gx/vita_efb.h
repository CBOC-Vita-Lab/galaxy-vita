#pragma once
#include "vita_material_cache.h"
#include "draw_spans.h"
#include "gx_copy_pixels.h"
#include "gx_live.h"

namespace gpu {
struct VitaEfbCache;
#define GPU_VITA_EFB_BLEND_SNAPSHOT 1
struct VitaEfbBlendSnapshot;
struct VitaEfbBlendSnapshotView {
    PortVitaGxmSurface* target=nullptr;
    PortVitaGxmProgram* program=nullptr;
    PortVitaGxmSurfaceSampleInfo sourceSample{},targetSample{};
};
// Mutable, full-size scratch for ordered GPU blending, not a published GX copy.
// Resolve/bind before producer scenes. The opaque lease pins source, scratch,
// descriptor storage and the cache-owned copy program through End AND Finish.
int resolveVitaEfbBlendSnapshot(VitaEfbCache*,PortVitaGxmSurface*,
    std::shared_ptr<const VitaEfbBlendSnapshot>&,std::string& error);
int bindVitaEfbBlendSnapshot(const VitaEfbBlendSnapshot*,PortVitaGxmDevice*,PortVitaGxmSurface*,uint32_t,uint32_t,
    VitaEfbBlendSnapshotView&,std::string& error);
struct VitaEfbClear {
    uint32_t left=0, top=0, width=0, height=0;
    uint32_t ar=0, gb=0, z=0; // verbatim GX clear bytes/24-bit depth
    // Complete BP 0x43: pixel type, Z16 compression and early-Z location.
    uint32_t colorWrite=0, alphaWrite=0, depthWrite=0, pixelFormat=0;
};

// Full-precision, reflected constant color/depth clear; not an EFB copy shader.
std::string genVitaEfbClearFragmentShader();
std::string genVitaEfbSnapshotFragmentShader(bool depth);
int createVitaEfbCache(PortVitaGxmDevice*,const VitaShaderLibrary&,VitaEfbCache**,std::string& error);
// Full native EFB dimensions, never the display size. Surface is borrowed and
// lease-pinned; only this cache can release its owned native target.
int resolveVitaEfb(VitaEfbCache*,uint32_t width,uint32_t height,PortVitaGxmSurface*&,
                   std::shared_ptr<const void>& lifetime,std::string& error);
// GPU rectangle clear of independently masked color/alpha/depth. Closed scene
// on success. Failed End/draw retains the target and staging, never auto-replays.
int clearVitaEfb(VitaEfbCache*,PortVitaGxmSurface*,const VitaEfbClear&,std::string& error);
// Only the clear portion after an actual successful copy. Does not fake copying.
int clearVitaEfbAfterCopy(VitaEfbCache*,PortVitaGxmSurface*,const SpanCopy&,std::string& error);
// Raw one-to-one GPU plane capture into a fresh leased target. Color preserves
// stored RGBA8; depth is encoded as high/middle/low Z24 bytes and alpha 255.
// Cache clear/reuse never mutates published captures. Source must be a mutable
// full-size EFB, not a prior capture. Borrowers must not render to captures.
// This does NOT implement GX copy filtering/format/RAM conversion or XFB.
// Nothing is published until End/Finish. Failure retains both planes/targets
// for explicit discard, which never repeats the capture.
int captureVitaEfb(VitaEfbCache*,PortVitaGxmSurface*,uint32_t left,uint32_t top,
                   uint32_t width,uint32_t height,bool depth,PortVitaGxmSurface*&,
                   std::shared_ptr<const void>& lifetime,std::string& error);
// Original CPU EFB access: one exact native color/Z24 capture, End/Finish,
// pixel-format quantization and alpha-read/Z16-compression conversion. Output
// remains untouched on failure; no source clear, RAM write or invented pixels.
int peekVitaEfb(VitaEfbCache*,PortVitaGxmSurface*,const RecorderPeek&,uint32_t&,std::string& error);
// Process only a successfully finished immutable raw capture, using its actual
// original-coordinate/plane provenance and physical color pitch. Native CPU
// reads are Finish-gated. No RAM writes, clear or implicit raw-plane fallback.
int processVitaEfbCapture(VitaEfbCache*,PortVitaGxmSurface*,const SpanCopy&,GxCopyPixels&,std::string& error);
// Scene-closed capture -> processing -> tiled texture or progressive YUYV XFB
// RAM commit -> optional clear. This does not present an XFB on the native display.
// The verified RAM window and ordered frame must come from the installed caller.
// Unlike preparatory APIs, a nonempty packet on ERROR means RAM ALREADY COMMITTED:
// stop frame submission and recover retained GPU work without repeating the copy.
// Never discard that commit marker or automatically retry a partially cleared copy.
int copyVitaEfbToMemory(VitaEfbCache*,GxCopyHistory*,PortVitaGxmSurface*,uint64_t frame,
                       const SpanCopy&,const GxCopyMemory&,std::shared_ptr<const GxCopyBytes>&,
                       std::string& error);
int discardVitaEfbClear(VitaEfbCache*);
// Recovery ordering: another renderer's staging cannot reset/retire while this
// cache owns the device's open scene. Query only from the native GPU owner.
bool vitaEfbSceneOpen(const VitaEfbCache*);
int collectVitaEfbs(VitaEfbCache*);
int releaseVitaEfbCache(VitaEfbCache**);
}
