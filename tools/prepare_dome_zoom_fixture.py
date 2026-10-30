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
    next(line for line in camera.splitlines() if line.startswith("static f32 sZoomNow =")),
    next(line for line in camera.splitlines() if line.startswith("static bool sZoomKnown =")),
    block(camera, "static f32 zoomedFovy("),
])
(out / "dome_zoom_camera_actual.inc").write_text("\n".join(parts) + "\n")
(out / "dome_zoom_target_actual.inc").write_text("\n".join([
    "static const f32 hZCheckAddDepth = 60.0f;",
    block(target, "inline void multProj("),
    block(target, "bool StarPointerTarget::isPointing("),
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
    )),
    block(perf, "int ZoomClamp("), block(perf, "int ZoomTenths("),
    block(perf, "int ZoomFromFile()"), block(perf, "int ZoomRead()"),
    block(perf, 'extern "C" float port_camera_zoom('),
    block(perf, 'extern "C" int port_zoom_adjust('),
]) + "\n")
