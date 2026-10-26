#!/usr/bin/env python3
"""Compile the unmodified steering consumers with host-only vector/animation stubs."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end] + "\n"


sphere = (root / "decomp/src/Game/Ride/SphereAccelSensorController.cpp").read_text()
ray = (root / "decomp/src/Game/Ride/SurfRay.cpp").read_text()
constants = sphere[sphere.index("    static const f32 sCoreBaseDegreeYZ"):sphere.index("};  // namespace")]
fixture = constants + "\n"
for name in ("inline f32 diffAngleAbs", "void SphereAccelSensorController::getPadAcceleration", "void SphereAccelSensorController::clacXY"):
    fixture += function(sphere, name)
fixture += function(ray, "void SurfRay::updateRotate")
Path(sys.argv[1]).write_text(fixture)
