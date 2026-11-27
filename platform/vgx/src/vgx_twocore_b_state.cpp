// GXV_FE_TWOCORE (vgx_twocore.h): vgx_state.cpp compiled a second time, into the second worker's instance of the front end (the namespace
// vgxb, VGX_CLONE_B). Its globals are that instance's own; what it shares with the player's goes through vgx_twocore.h.
#define VGX_CLONE_B 1
#define vgx vgxb
#include "vgx_state.cpp"
