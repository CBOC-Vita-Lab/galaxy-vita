// bglayer25 (11 Nov 2026): the census-only stage of GXV_BGLAYER. The layer cache itself (BG_BEGIN / BG_END / BG_REUSE, the backdrop draw) comes with PATCH2.diff, which
// replaces this file. Here: the marker dispatch for kind 28 (the census), and the view hand-over as a no-op. Included into vgx_draw.cpp, namespace vgx.
void BgSetView(uint32_t, const float[12], float, float) {}
void BgLayerCounters(uint64_t out[8]) { for (int i = 0; i < 8; ++i) out[i] = 0; }
void BgMarker(uint32_t kind, uint32_t payload) {
    if (kind == 28u && (BgLayerMode() & 2) != 0) BgCatMarker(payload & 0xFFu);
}
