// Force-included into every decompiled translation unit (-include port/compat.h).
// Bridges CodeWarrior/PowerPC assumptions to a 64-bit little-endian Clang target.
#pragma once

#ifndef TARGET_PC
#define TARGET_PC 1
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
// Declare the C library's printf family (including bionic's FORTIFY inline
// wrappers) before the renames below, so the macros only affect game code.
#include <stdio.h>
#include <float.h>
#if defined(__linux__) && !defined(__ANDROID__)
// Parse libc/libc++ declarations before the game's UTF-16 name remapping.
#ifdef __cplusplus
#include <cwchar>
#else
#include <wchar.h>
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// 16-bit wide strings.  CodeWarrior's wchar_t is 16 bits and the game stores
// UTF-16 text in its data files; we build with -fshort-wchar, so the host
// libc's (32-bit) wide-string functions must not be used.
// ---------------------------------------------------------------------------
size_t port_wcslen(const wchar_t* s);
wchar_t* port_wcsncpy(wchar_t* dst, const wchar_t* src, size_t n);
wchar_t* port_wcscpy(wchar_t* dst, const wchar_t* src);
int port_wcscmp(const wchar_t* a, const wchar_t* b);
int port_wcsncmp(const wchar_t* a, const wchar_t* b, size_t n);
wchar_t* port_wcschr(const wchar_t* s, wchar_t c);
wchar_t* port_wcscat(wchar_t* dst, const wchar_t* src);
int port_swprintf(wchar_t* dst, size_t n, const wchar_t* fmt, ...);
int port_vswprintf(wchar_t* dst, size_t n, const wchar_t* fmt, va_list args);

#define wcslen port_wcslen
#define wcsncpy port_wcsncpy
#define wcscpy port_wcscpy
#define wcscmp port_wcscmp
#define wcsncmp port_wcsncmp
#define wcschr port_wcschr
#define wcscat port_wcscat
#define swprintf port_swprintf
#define vswprintf port_vswprintf

// CodeWarrior MSL printf semantics for game code (NULL "%s" prints nothing,
// "l" is 32-bit, "%ls" is 16-bit); see platform/src/port/msl_printf.cpp.
int port_vsnprintf(char* dst, size_t n, const char* fmt, va_list args);
int port_snprintf(char* dst, size_t n, const char* fmt, ...);
int port_vsprintf(char* dst, const char* fmt, va_list args);
int port_sprintf(char* dst, const char* fmt, ...);
#ifndef PORT_PLATFORM_CODE
#define vsnprintf port_vsnprintf
#define snprintf port_snprintf
#define vsprintf port_vsprintf
#define sprintf port_sprintf
#endif

// Renderer hooks called from game code (platform/src/gx/gx_recorder.cpp):
// markers around HUD drawing and the main camera, both kept in order with
// the GX commands issued before them.
void port_gx_marker(unsigned int kind);
// 1 on targets where drawing is the scarce resource (PS Vita; PETARI_HANDHELD=1 elsewhere, to
// test the same paths on the desktop): game code then leaves out drawing that cannot be seen.
int port_handheld(void);
// Frame-time statistics (see port/port.h).
void port_perf_frame_begin(void);
void port_perf_frame_work_done(void);
void port_gx_camera(const float* camera);  // PORT_GX_CAMERA_WORDS words
// Called where the game steers with the Wii remote's tilt (Star Ball, Ray
// surfing).  While it keeps being called, the controller's orientation feeds
// the accelerometer; otherwise the remote reads as held level and still, so
// ordinary pointing never registers as a shake.  neutralPitchDeg: remote
// pitch (0 = level, 90 = pointing up) that a level-held controller stands
// for (platform/src/input/wpad.cpp).
void port_input_use_tilt(float neutralPitchDeg);
#define PORT_GX_MARK_HUD_BEGIN 1
#define PORT_GX_MARK_HUD_END 2
#define PORT_GX_MARK_SKY_BEGIN 3  // skies drawn around the game camera
#define PORT_GX_MARK_SKY_END 4
#define PORT_GX_MARK_PLAYER_BEGIN 5  // the player's model and what it carries (motion vectors of their own)
#define PORT_GX_MARK_PLAYER_END 6
#define PORT_GX_MARK_SCENE_DEPTH 7  // the 3D scene's depth is complete (image effects and a Z clear follow)
#define PORT_GX_MARK_POINTER_BEGIN 8  // the pointer's 2D cursor (inside the HUD markers)
#define PORT_GX_MARK_POINTER_END 9
// gpu23 (GXV_INDCAP_SKIP): the next EFB copy is the "Indirect" screen capture (CaptureScreenDirector, no other capture timing asked
// for): only draws of the same picture that come after it read what it makes (the next picture captures again before its readers).
#define PORT_GX_MARK_TRANSIENT_COPY 10
// The game's draw phase from here on (decomp GameScene::draw3D / draw2D, SceneFunction), in bits 8-15: GXV_GEOPASS's census
// (vgx_geopass.inc). Written into the vgx GX stream only with GXV_GEOPASS on; ignored by every other renderer.
#define PORT_GX_MARK_PHASE 11
// hud24 (GXV_HUD_NATIVE): the 2D draws of a scene that draws its layouts outside the HUD markers (ScenarioSelectScene's layouts, PlayTimerScene's):
// the same meaning as HUD_BEGIN / HUD_END for the native-resolution 2D layer, and nothing else (no edge smoothing rectangles). Written into the vgx
// GX stream only with GXV_HUD_NATIVE on; ignored by every other renderer.
#define PORT_GX_MARK_HUDN_BEGIN 12
#define PORT_GX_MARK_HUDN_END 13
// prewarm24 (GXV_PREWARM): the draws between these are resolved by the vgx front end for their shader programs and not drawn; the front end puts
// its register state back at the end (vgx_draw.cpp WarmScopeMarker). Written into the vgx GX stream only with GXV_PREWARM on; ignored by every
// other renderer.
#define PORT_GX_MARK_WARM_BEGIN 14
#define PORT_GX_MARK_WARM_END 15
// layoutwarm24 (GXV_LAYOUTWARM_LOG): the draws between these (the kind in bits 0-7, the layout's census id in bits 8-23) are one layout's draw in play: the
// front end counts the program variants and textures it makes for it. Written into the vgx GX stream only with GXV_LAYOUTWARM_LOG on; ignored by every
// other renderer.
#define PORT_GX_MARK_LAYOUT_BEGIN 19
#define PORT_GX_MARK_LAYOUT_END 20
// bglayer25 (GXV_BGLAYER=2, the census): the draws that follow, up to the next marker of this kind with id 0, belong to one draw category (bits 8-15: 1 + the
// MR::DrawBufferType, or 64 + the MR::DrawType). The vgx front end tallies draws and indices by it. Written into the vgx GX stream only with GXV_BGLAYER on
// (and GXV_DRAWPIPE=0); ignored by every other renderer.
#define PORT_GX_MARK_BGCAT 28
// bglayer25 (GXV_BGLAYER=1, the far-background layer cache): BG_BEGIN opens the far layer's draws of a picture that renders it (nothing else is drawn between it and BG_END
// but the far layer, first in the picture after the clear); BG_END (the view slot in bits 8-23) closes it: the front end copies the picture so far into the layer cache and
// remembers the slot's view; BG_REUSE (the slot in bits 8-23) stands where the far layer's draws would be in a picture that leaves them out: the front end draws the cache as
// the backdrop, turned from the view it was made for to this slot's. The game writes the slot's view first (port_vgx_bglayer_view). Written into the vgx GX stream only
// with GXV_BGLAYER on; ignored by every other renderer.
#define PORT_GX_MARK_BG_BEGIN 29
#define PORT_GX_MARK_BG_END 30
#define PORT_GX_MARK_BG_REUSE 31
// shadow25 (GXV_SILH_GATE): written just before the silhouette fill's capture, hint in bits 8-15: 1 the player's silhouette cannot show in this picture,
// 2 it may. Written into the vgx GX stream only with GXV_SILH_GATE on; ignored by every other renderer.
#define PORT_GX_MARK_SILH_HINT 32
// precomp26 (GXV_STAGE_ENUM): inside a PORT_GX_MARK_WARM_BEGIN / _END scope, in place of a J3D shape's geometry: the vgx front end resolves the draw the state would make (no
// vertices, no textures, nothing recorded) and hands its program variant to the back end's enumeration. Bits 8-15: the highest position-matrix row the shape's vertices
// name (0xFF none), bits 16-23: the vertex count of the shape's smallest primitive (0 not known). Written into the vgx GX stream only with GXV_STAGE_ENUM on; ignored by
// every other renderer.
#define PORT_GX_MARK_STAGE_ENUM_DRAW 33
// hrw26 (GXV_HRW): a picture the render thread keeps for a second presentation; value = (pair id << 8) as vgx::kHrwMarker carries it.
#define PORT_GX_MARK_HRW 34
// hrw27 (GXV_HRW): the owner of the draws that follow (value = id << 8 | phase, in bits 8-23 of the marker; 0 = nobody's), and the end of a pair's partial draw (value = the pair's number). Written into the vgx GX
// stream only with GXV_HRW 1, 2 or 4; ignored by every other renderer. The ids and phases are vgx_hrw.h's OwnerIdValue / OwnerPhase.
#define PORT_GX_MARK_OWNER 35
#define PORT_GX_MARK_FRESH_END 36
#define PORT_GX_MARK_HRW_PLAIN 37 // Pair A promises not to ask for fresh geometry on B.
// hotprof (GXV_HOTPROF_TEVCOL=2): the verify pair (register high half, then low half) written before a TEV colour write the game would leave out.
// Kinds 28/29 are the BGLAYER markers above: one kind must have one meaning (they were shared until 20 Nov 2026).
#define PORT_GX_MARK_TEVCOL_REG 38
#define PORT_GX_MARK_TEVCOL_VAL 39
// bloom37 (GXV_BLOOM_BRACKET): the normal bloom's scene bracket (BloomEffect::preDraw's capture of the scene into _24 and black fill, postDraw's put-back). The sub-kind is in bits 8-15:
// 1 the next EFB copy is the bracket's scene capture, 2 / 3 the black fill's draws begin / end, 4 / 5 the put-back draw begins / ends, 6 no glow source drew (the bracket is dead: the capture, the fill
// and the put-back restore what was there). Written into the vgx GX stream only with GXV_BLOOM_BRACKET on (and GXV_HRW 0); ignored by every other renderer.
#define PORT_GX_MARK_BLOOM_BRACKET 40
#define PORT_BB_CAPTURE 1
#define PORT_BB_FILL_BEGIN 2
#define PORT_BB_FILL_END 3
#define PORT_BB_PUT_BEGIN 4
#define PORT_BB_PUT_END 5
#define PORT_BB_DEAD 6
#define PORT_HRW_OWN_MARIO 1
#define PORT_HRW_OWN_POINTER 2
#define PORT_HRW_OWN_SKY 0xFE
#define PORT_HRW_PH_PLAYER_DRAW 1
#define PORT_HRW_PH_PLAYER_DECO_OPA 2
#define PORT_HRW_PH_PLAYER_DECO_XLU 3
#define PORT_HRW_PH_POINTER 4
#define PORT_HRW_PH_SKY 5
#define PORT_GX_OWNER(id, phase) (PORT_GX_MARK_OWNER | (((((unsigned)(id)) << 8) | (unsigned)(phase)) << 8))
#define PORT_GX_CAMERA_WORDS 33
// Word 25 of the camera record: presentation hints.
#define PORT_GX_CAMERA_DIORAMA 1     // gameplay: show the scene as a VR diorama
#define PORT_GX_CAMERA_POINTER_UI 2  // the Wii pointer is used on 2D menus
#define PORT_GX_CAMERA_PLAYER 4      // words 27-29 hold the player's centre, 30-32 its up (against gravity), in world space
#define PORT_GX_CAMERA_OCCLUDED 8    // level geometry lies between the VR eye and the player
#define PORT_GX_CAMERA_GROUNDED 16   // the player stands on the ground (not jumping, falling or swimming)
#define PORT_GX_CAMERA_CENTRE_PLAYER 32  // the diorama centres the player (else the point the camera watches)

// The headset's view in game world space, for object culling while the VR
// camera is active: a camera-to-world matrix (3x4 rows; columns are the
// left, up and forward axes and the position) and the tangents of the half
// field of view.  Returns 0 when not rendering in VR.
int port_vr_cull_view(float* cameraMtx, float* tanHalfX, float* tanHalfY);
// The diorama's axes in game world space while it is shown: right, up
// (gravity at the camera's focus) and forward (away from the player).
// Mario's movement follows them instead of the game camera's screen axes,
// which differ once the camera turns upside down: the diorama always shows
// the world upright.  Returns 0 when the diorama is not shown.
int port_vr_move_axes(float* right, float* up, float* forward);
// Nonzero while the player aims the pointer into the VR diorama; the game's
// 2D cursor is hidden then (the VR layer draws a laser and reticle).
int port_vr_pointer_in_world(void);
// The pointing controller's aim ray in game world space (origin[3] and unit
// dir[3]) while it points into the diorama; returns 0 otherwise.  Star
// pointer targets are hit-tested against it instead of the 2D cursor.
int port_vr_pointer_ray(float* origin, float* dir);
// How far along that ray the pointer reaches (game units): its first map
// surface, or open space where star bits aim past the player.  Reported each
// frame; the VR layer draws the laser that long.
void port_vr_pointer_reach(float distance);
// How far in front of the game camera the point under the pointer's 2D
// cursor lies (game units; the first map surface on the camera's ray through
// it, or the far plane).  Reported each frame; the stereoscopic screen draws
// the cursor at that depth, on what it points at.
void port_vr_pointer_depth(float depth);
// Called each frame the game's pointer touches a target (`id` identifies it);
// the VR layer pulses the reticle and ticks the controller on a new touch.
void port_vr_pointer_touched(unsigned long long id);
// Screen wipes, reported each frame one is drawn: kind 0 = fade to `rgb`,
// 1 = ring closing on the player, 2 = ring closing on the screen centre;
// closed = 0 (open) .. 1 (covered).  While the VR diorama is shown
// (port_vr_diorama), the VR layer draws the wipe across the whole view and
// the game skips its 2D version, which would only cover the HUD panel.
void port_vr_wipe(int kind, float closed, unsigned int rgb);
int port_vr_diorama(void);
// Scripted transits: launch and sling stars report themselves from the
// launch until they let go of Mario, pipes from Mario stepping in until he
// jumps out.  The game camera follows those with its own framing (a flight
// across space, or the whole planet a pipe runs through), so while the
// player is still bound to that object the VR layer shows the game camera
// on the virtual screen instead of the diorama.
void port_vr_transit_begin(const void* host);
void port_vr_transit_end(const void* host);  // only if `host` began it
const void* port_vr_transit_host(void);
// anticip32 (GXV_ANTICIPATE, default off: the call returns at once): the game announces a moment that
// is known to be heavy or to start a fast camera, so the scaling engine can give up a little far
// detail BEFORE it and put it back after (platform/include/port/anticipate.h; perf.cpp "anticip32").
// kind: 1 launch star, 2 sling star, 4 boss battle, 5 power-up change; the moment begins in about
// lead_ms and lasts about last_ms. Drawing only: it changes no game value and no timing.
void port_anticipate_event(int kind, int lead_ms, int last_ms);
// The pause menu reports each frame whether its buttons wait for a choice;
// meanwhile the VR layer shows its settings panel beside it.
void port_vr_pause_menu(int selecting);

// Holding A skips cutscenes (platform/src/port/cutscene_skip.cpp).  The game
// reports each update which kind of cutscene runs; once A has been held long
// enough, port_skip_take() returns true once for that kind (movies, the
// galaxy intro camera and fly-in end through their own skip paths), and an
// in-engine demo or a dialogue is fast-forwarded (port_skip_fast_forwarding)
// until it ends.  port_skip_interrupt(): a choice waits for the player; it
// stops a fast-forward (returns 1 then: this update's A was not the
// player's).
#define PORT_SKIP_NONE 0
#define PORT_SKIP_MOVIE 1    // a THP movie
#define PORT_SKIP_OPENING 2  // the camera tour when a galaxy starts
#define PORT_SKIP_STARTER 3  // Mario flying in to a galaxy
#define PORT_SKIP_DEMO 4     // an in-engine cutscene
#define PORT_SKIP_TALK 5     // a dialogue outside a cutscene (most people's talk)
void port_skip_context(int kind);
int port_skip_movie_playing(void);  // the last context was a movie (any thread)
int port_skip_take(int kind);
// Whether a disc file (a path inside the game's file system, such as "/MovieData/PrologueB.thp") is on the data
// card: a movie that is not there is skipped (platform/src/dvd/dvd.cpp), the game would wait for it forever.
int port_disc_file_present(const char* path);
// START (+) or a touch of the screen answers the title screen's "press A and B" (platform/src/input/wpad.cpp).
int port_title_confirm(void);
int port_skip_fast_forwarding(void);
int port_skip_interrupt(void);
// A scene change began during a fast-forward (the skipped cutscene ends in
// another stage, or in the star select): the fast-forward stops, and the
// change runs at the normal pace, drawn.
void port_skip_scene_change(void);
// The VR controller's menu buttons (X, Menu) ask for the pause menu with a
// single press (wpad.cpp): the game takes the request at the first moment it
// allows the pause, within 1.5 s of the press (port_input_take_pause_request
// returns 1 once), and drops requests made before its pause menu closed.
void port_input_request_pause(void);
int port_input_take_pause_request(void);
void port_input_discard_pause_request(void);
// The invert_camera setting: the D-pad's left and right swapped where they
// turn the game camera round Mario (and nowhere else: pages and menus keep
// theirs), so pushing the right stick right turns the view to the right.
int port_input_camera_inverted(void);
int64_t port_host_time_ns(void);
void port_host_sleep_ns(int64_t ns);
// Delivers pending interrupts (a safe point; only from the thread holding the
// CPU).  Interrupts are otherwise taken when they are re-enabled, in blocking
// OS calls and when the CPU idles, so a loop that spins until an interrupt
// handler changes something must call this (on the console the interrupt
// would simply preempt it).
void port_irq_poll(void);

// Logging / fatal errors from the port layer.
void port_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void port_fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
// (fmt32: also declared in port/diagnostics.h; repeated here, in the header every game and runtime unit is given, so that the compiler checks the
// format of every call, also in units that only carry their own plain declaration of it)
void port_log_summary(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

// CodeWarrior-only keywords / pragmas that may leak into headers.
#ifndef _WIN32
#define __declspec(x)
#endif
#define __option(x) 0

#ifdef __cplusplus
#include "port/msl_ext.hpp"
#endif

// MSL <cmath> helpers (MSL's M_PI is a float literal).
#define DEG_TO_RAD(degrees) (degrees * (3.14159265358979323846f / 180.0f))
#define RAD_TO_DEG(radians) (radians * (180.0f / 3.14159265358979323846f))
