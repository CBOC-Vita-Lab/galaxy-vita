#!/usr/bin/env python3
"""Compile the production capture helper and its actual FallOutField call.

Only the texture and framebuffer interfaces are faked. The helper body and
caller statement are extracted verbatim from checked-out production sources.
The captured operation is checked against the intended full-screen rectangle.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    helper_path = ROOT / "decomp/src/Game/Screen/ImageEffectLocalUtil.cpp"
    actor_path = ROOT / "decomp/src/Game/MapObj/FallOutFieldDraw.cpp"
    helper_source = helper_path.read_text()
    actor_source = actor_path.read_text()
    helper = function(helper_source, "void ImageEffectLocalUtil::capture(")
    call_start = actor_source.index("#if defined(TARGET_PC)", actor_source.index("void FallOutFieldDraw::draw()"))
    call = actor_source[call_start:actor_source.index("#endif", call_start) + len("#endif")]
    env_int = function(actor_source, "long EnvInt(")
    other_calls = re.findall(r"ImageEffectLocalUtil::capture\(pScreenAlphaTexture[34],.*?;", actor_source)
    assert len(other_calls) == 2
    reference = "ImageEffectLocalUtil::capture(pScreenAlphaTexture2, 1, 0, GX_CTF_A8, true, 0);"
    (args.output / "inputs.json").write_text(json.dumps({
        str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in (helper_path, actor_path)
    }, indent=2) + "\n")
    preamble = r'''
#include <cstdio>
#include <cstdlib>
using s32=int; using u8=unsigned char; using GXTexFmt=unsigned;
constexpr GXTexFmt GX_CTF_A8=0x27;
struct JUTTexture {
    int x=-1,y=-1; GXTexFmt format=0; bool half=false; u8 clear=255;
    void capture(int px,int py,GXTexFmt f,bool h,u8 c) {
        x=px;y=py;format=f;half=h;clear=c;
    }
};
namespace MR {
int getFrameBufferWidth() { return 640; }
int getFrameBufferHeight() { return 456; }
}
namespace ImageEffectLocalUtil {
void capture(JUTTexture*,s32,s32,GXTexFmt,bool,u8);
}
'''
    results = {}
    cases = [("unset", None, True, True), ("zero", "0", True, True),
             ("invalid", "invalid", True, True), ("enabled", "1", True, False),
             ("nonport", "1", False, True), ("reference", None, True, False)]
    for name, flag, target_pc, expect_ub in cases:
        statement = reference if name == "reference" else call
        source = preamble + env_int + helper + r'''
void firstCapture(JUTTexture* pScreenAlphaTexture2) {
''' + statement + r'''
}
int main() {
    JUTTexture texture;
    firstCapture(&texture);
    std::printf("capture=(%d,%d) format=%u half=%d clear=%u\n",
                texture.x,texture.y,texture.format,texture.half,texture.clear);
    if(texture.x!=0 || texture.y!=0 || texture.format!=GX_CTF_A8 || !texture.half || texture.clear) return 1;
    setenv("GXV_CLIP_CAPTURE_FIX", "0", 1);
    firstCapture(&texture);
    if(texture.x!=0 || texture.y!=0) return 2;
    JUTTexture* pScreenAlphaTexture3=&texture;
    JUTTexture* pScreenAlphaTexture4=&texture;
''' + other_calls[0] + r'''
    if(texture.x!=0 || texture.y!=0 || texture.format!=GX_CTF_A8 || !texture.half || texture.clear) return 3;
''' + other_calls[1] + r'''
    if(texture.x!=0 || texture.y!=114 || texture.format!=GX_CTF_A8 || texture.half || texture.clear) return 4;
    std::puts("PASS: first capture, read-once flag, half-size capture, tile4 capture");
    return 0;
}
'''
        cpp = args.output / f"{name}.cpp"
        exe = args.output / name
        cpp.write_text(source)
        defines = ["-DTARGET_PC=1"] if target_pc else []
        subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++17", "-O1", "-g", *defines,
                        "-fsanitize=undefined", "-fno-sanitize-recover=all", str(cpp), "-o", str(exe)], check=True)
        env = os.environ.copy()
        env.pop("GXV_CLIP_CAPTURE_FIX", None)
        if flag is not None:
            env["GXV_CLIP_CAPTURE_FIX"] = flag
        result = subprocess.run([str(exe)], env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (args.output / f"{name}.log").write_text(result.stdout)
        ok = (result.returncode != 0 and "division by zero" in result.stdout) if expect_ub else result.returncode == 0
        results[name] = ok
        print(f"{name}: exit={result.returncode}\n{result.stdout}", end="")
    print(json.dumps(results))
    return int(not all(results.values()))


if __name__ == "__main__":
    raise SystemExit(main())
