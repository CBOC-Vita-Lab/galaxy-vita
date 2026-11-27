// GXV_STREAM_CENSUS=<n> (lighter25, 11 Nov; default 0): the game's side of the stream census (platform/vgx/src/vgx_census.inc).
// Every n-th picture the game marks its GX stream (BP 0xFF writes, as every PORT_GX_MARK_*): BEGIN right after the display copy that
// ends the picture before (decomp GXFrameBuf.c GXCopyDisp), the source of what follows (a draw list of SceneFunction.cpp's CategoryList,
// a part of GameScene::draw), and each J3D material packet's model with the projected size of its actor's clipping sphere. Nothing is
// written in the other pictures; with 0, nothing anywhere (one load per draw list / material packet / display copy).
// Implemented in platform/src/port/perf.cpp. Game thread (or the thread that writes the GX FIFO: GXV_DRAWPIPE's draw thread).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
extern int port_sc_turn;  // nonzero while the picture being drawn is counted (read directly: one load)
void port_stream_census_copy(void);                       // GXCopyDisp, after its copy command
void port_stream_census_src_begin(int id);                // a source begins (nested: a stack)
void port_stream_census_src_end(void);                    // and ends: the outer one again
void port_stream_census_model_begin(const void* pModelData);                    // J3DMatPacket::draw: the J3DModelData (no size)
void port_stream_census_model_begin2(const void* pModelData, unsigned stamp);   // the same, with the model's size stamp (port_stream_census_stamp)
void port_stream_census_model_end(void);
void port_stream_census_model_note(const void* pModelData, const char* pName);  // ResourceHolder: a model data's file name
// ClippingActorHolder: the size code of a judged actor's clipping sphere (centre distance from the camera, radius), kept on its model
// for the next counted picture (0: no stamp)
int port_stream_census_judging(void);  // the next picture drawn is counted (stamps are wanted this turn)
unsigned port_stream_census_stamp(float radius, float dist, float tanHalfFovy);
#ifdef __cplusplus
}
#endif
