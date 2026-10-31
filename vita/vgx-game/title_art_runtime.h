#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Starts the low-priority worker that, once the real title screen has been fully shown, saves the presented frame to
// ux0:data/smg/art/title.rgba and makes the loading-screen, Settings and home-screen pictures from it (port/title_art.h).
// GXV_TITLEART: 0 off, 1 (default) once, 2 a new capture at every start. Call once, after the game has been started.
void port_title_art_start(void);
#ifdef __cplusplus
}
#endif
