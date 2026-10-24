// The vgx renderer (platform/vgx) as the game's GPU: the glue between the port's GX FIFO and
// the renderer's front end, and the thread that owns its back end.
//
// vgx reads the big-endian command stream the game's own GX library writes (FIFO bytes,
// display lists, register loads) and turns it into command buffers for a GLES2-class back
// end: Mesa on the desktop (to develop against the upstream renderer's pictures), vitaGL on
// the PS Vita.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// Selects the renderer at start-up (PETARI_BACKEND=vgx|legacy; the default is vgx where the
// legacy GLES 3.2 renderer does not exist). Replaces port_gx_use_recorder_backend().
void port_gx_select_backend(void);
int port_vgx_active(void);
// FIFO bytes for the renderer. Returns what was consumed (whole commands only).
size_t port_vgx_consume(const uint8_t* data, size_t len);
// The renderer's front end on a thread of its own ("player", GXV_PLAYER=1, the default): the
// game thread writes into one of two staging segments and only publishes how far it has
// written; the player parses behind it. port_vgx_publish returns where writing continues (the
// same position, or one in the other segment); flags: 1 the segment is full, 2 the bytes end
// with a synchronisation command (draw done, draw-sync token).
int port_vgx_player(void);
void port_vgx_stream_setup(uint8_t* segment0, uint8_t* segment1, size_t size);
uint8_t* port_vgx_publish(uint8_t* writePosition, int flags);
// The game made [p, p + n) visible to the GPU (data-cache flush / store) or a bulk write
// landed there (disc, save file, movie decode): cached decodes of it are checked again.
void port_vgx_notify_write(const void* p, size_t n);
// Desktop screenshots: the next presented frame as a PNG. Returns its frame number, 0 if
// none came within the wait.
unsigned long long port_vgx_capture(const char* path, int scale);
unsigned long long port_vgx_frames(void);
#ifdef __cplusplus
}
#endif
