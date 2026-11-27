"""Derive the host arithmetic model from the actual present shader, not a second handwritten shader."""
import hashlib
import json
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
backend = (root / "platform/vgx/src/vgx_backend_gl.cpp").read_text()
start = backend.index("bool EnsureSharpenProgram() {")
backend = backend[start:backend.index("\n} // namespace", start)]
reference = (root / "tests/vgx_present_reference.inc").read_text()


def c_string(source, name):
    found = re.search(r"static const char\* " + name + r' =\s*((?:"(?:\\.|[^"\\])*"\s*)+);', source)
    assert found, name
    return "".join(json.loads(token) for token in re.findall(r'"(?:\\.|[^"\\])*"', found[1]))


def calls(source, prefix):
    result = []
    offset = 0
    while (start := source.find(prefix, offset)) >= 0:
        end = source.index("(", start) + 1
        depth = 1
        while depth:
            depth += (source[end] == "(") - (source[end] == ")")
            end += 1
        result.append(source[start:end])
        offset = end
    return result


def modern(source, vertex=False):
    return ("#version 300 es\n" + source.replace("attribute ", "in ")
            .replace("varying ", "out " if vertex else "in ").replace("texture2D(", "texture(")
            .replace("gl_FragColor", "outColor").replace("void main()", "void main()" if vertex
                                                       else "out vec4 outColor;\nvoid main()"))


vs, baseline, candidate = (c_string(backend, name) for name in ("vs", "fs", "fsHalf"))
assert vs == c_string(reference, "vs") and baseline == c_string(reference, "fs")
assert calls(candidate, "texture2D(") == calls(baseline, "texture2D(")
assert len(calls(candidate, "texture2D(")) == 5
assert "vec3 c = texture2D(u_src, uv).rgb;" in candidate
assert "vec2 uv = clamp(v_uv, u_lim.xy, u_lim.zw);" in candidate
assert "vec3 result = vec3(c) + (vec3(c) - vec3(b) * 0.25) * u_p.z;" in candidate
assert "gl_FragColor = vec4(result, 1.0);" in candidate
for uniform in ("u_size", "u_lim", "u_p"):
    assert f"uniform vec4 {uniform};" in candidate
samples = calls(candidate, "RGBV(texture2D(")
assert len(samples) == 4
emulated = candidate.replace("#ifdef VITAGL\n#define RGBV half3\n#else\n#define RGBV vec3\n#endif\n", "")
for sample in samples:
    emulated = emulated.replace(sample, sample.replace("RGBV(", "H("))
emulated = emulated.replace("RGBV ", "vec3 ")
begin = emulated.index("  vec3 b =")
end = emulated.index(";", begin)
taps = [s.replace("RGBV(", "H(") for s in samples]
emulated = emulated[:begin] + f"  vec3 b = H(H(H({taps[0]} + {taps[1]}) + {taps[2]}) + {taps[3]})" + emulated[end:]
# The real upload always sets u_p.w to zero. This opaque XOR retains the half round trip.
half = ("vec3 H(vec3 x) { uint barrier=floatBitsToUint(u_p.w); return vec3("
        "unpackHalf2x16(packHalf2x16(x.xy)^barrier), "
        "unpackHalf2x16(packHalf2x16(vec2(x.z,0.0))^barrier).x); }\n")
emulated = modern(emulated).replace("void main()", half + "void main()")


def high_sampler(source):
    # GLES defaults samplers to lowp. This separate model explicitly exercises full filtered samples.
    return source.replace("uniform sampler2D u_src;", "uniform highp sampler2D u_src;")


sources = {"pixelVS": modern(vs, True), "pixelFloatFS": modern(baseline),
           "pixelStrictFloatFS": high_sampler(modern(baseline)), "pixelHalfFS": high_sampler(emulated)}
Path(sys.argv[1]).write_text("\n".join(f"static const char* {name} = {json.dumps(value)};" for name, value in sources.items()) + "\n")
print("PASS actual-source present extraction", hashlib.sha256(candidate.encode()).hexdigest(), "native_gpu_execution=false")
