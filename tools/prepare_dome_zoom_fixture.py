#!/usr/bin/env python3
"""Extract production camera/target math; no game assets or save files needed."""
import pathlib
import sys


def block(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


root = pathlib.Path(__file__).resolve().parents[1]
out = pathlib.Path(sys.argv[1])
camera = (root / "decomp/src/Game/Camera/CameraContext.cpp").read_text()
target = (root / "decomp/src/Game/Screen/StarPointerTarget.cpp").read_text()
matrix = (root / "decomp/libs/JSystem/include/JSystem/JGeometry/TMatrix.hpp").read_text()
perf = (root / "platform/src/port/perf.cpp").read_text()
parts = []
if "static long EnvInt(" in camera:
    parts.append(block(camera, "static long EnvInt("))
parts.extend([
    block(camera, "static bool inGameScene()"),
    block(camera, "static bool inCutscene()"),
    *([block(camera, "static const char* zoomSetAsideReason()")] if "static const char* zoomSetAsideReason()" in camera else []),
    next(line for line in camera.splitlines() if line.startswith("static f32 sZoomNow =")),
    next(line for line in camera.splitlines() if line.startswith("static bool sZoomKnown =")),
    block(camera, "static f32 zoomedFovy("),
])
(out / "dome_zoom_camera_actual.inc").write_text("\n".join(parts) + "\n")
(out / "dome_zoom_target_actual.inc").write_text("\n".join([
    "static const f32 hZCheckAddDepth = 60.0f;",
    block(target, "inline void multProj("),
    block(target, "bool StarPointerTarget::isPointing("),
    block(target, "bool StarPointerTarget::isPointingEdge("),
    block(target, "f32 StarPointerTarget::calcRadius2d("),
    block(target, "bool StarPointerTarget::calcScreenPositionFromView("),
]) + "\n")
(out / "dome_zoom_projection_actual.inc").write_text(
    block(matrix, "void makePerspective(") + "\n"
)
(out / "dome_zoom_settings_actual.inc").write_text("\n".join([
    *(next(line for line in perf.splitlines() if line.startswith(prefix)) for prefix in (
        "constexpr int kZoomMin", "std::atomic<int> sZoomTenths", "std::atomic<unsigned> sZoomChange",
        "std::string sZoomFile", "static std::atomic<unsigned> sZoomLimitHits",
        # settingsnative: the other keys of settings.ini and the live setters
        "std::atomic<int> sRearAim", "std::atomic<int> sStickPointer", "std::atomic<int> sFreeCamera",
        "std::atomic<int> sRightStickMode", "std::atomic<int> sRightStickEnv",
        "std::atomic<int> sCameraSpeed", "std::atomic<int> sCameraSpeedEnv", "constexpr int kCameraSpeedLevels", "constexpr int kCameraSpeedDefault",
        "std::atomic<int> sStarBitReach", "constexpr int kStarBitReachLevels",
        "std::atomic<int> sTapJump", "std::atomic<int> sInvertCamera",
        'extern "C" void port_input_set_camera_inverted', 'extern "C" void port_input_set_camera_invert_mask',
        'extern "C" int port_vita_input_external_pad',
    )),
    block(perf, "int ZoomClamp("), block(perf, "int ZoomTenths("),
    block(perf, "int ZoomFromFile()"), block(perf, "int ZoomRead()"),
    block(perf, "int IntOptionFromFile("), block(perf, "int OptionFromFile("),
    block(perf, "int RightStickEnvOverride()"), block(perf, "int CameraSpeedEnvOverride()"),
    block(perf, "int RearAimRead()"), block(perf, "int RightStickFileModeRead()"), block(perf, "int RightStickModeRead()"),
    block(perf, "int StickPointerRead()"), block(perf, "int FreeCameraRead()"),
    block(perf, "int CameraSpeedFileRead()"), block(perf, "int CameraSpeedRead()"),
    block(perf, "int StarBitReachFromFile()"), block(perf, "int StarBitReachRead()"), block(perf, "int TapJumpRead()"),
    block(perf, "int InvertCameraRead()"), block(perf, "void ApplyInvertCamera("),
    block(perf, "int ZoomFileText("),
    *(block(perf, 'extern "C" int ' + name + '(') for name in (
        "port_rear_aim_enabled", "port_stick_pointer_enabled", "port_star_bit_reach", "port_zoom_tenths",
        "port_tap_jump_enabled", "port_tap_jump_set", "port_invert_camera_enabled", "port_invert_camera_set",
        "port_free_camera_enabled", "port_right_stick_mode", "port_right_stick_mode_set",
        "port_camera_speed", "port_camera_speed_set",
        "port_rear_aim_set", "port_stick_pointer_set", "port_star_bit_reach_set")),
    block(perf, 'extern "C" float port_camera_zoom('),
    block(perf, 'extern "C" int port_zoom_adjust('),
]) + "\n")
