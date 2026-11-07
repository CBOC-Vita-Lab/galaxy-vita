#!/usr/bin/env python3
"""Extract actual settings/save ownership paths; replace only their boundaries."""
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
runtime = (root / "vita/vgx-game/settings_runtime.cpp").read_text()
sequence = (root / "decomp/src/Game/System/SaveDataHandleSequence.cpp").read_text()
functions = (root / "decomp/src/Game/System/GameSystemFunction.cpp").read_text()
nand = (root / "platform/src/nand/nand.cpp").read_text()
manager = (root / "decomp/src/Game/System/NANDManagerThread.cpp").read_text()
parts = [
    block(runtime, "bool Enabled()"),
    block(runtime, "bool Relaunch()"),
    block(runtime, "struct RuntimeLease {") + ";",
    block(runtime, 'extern "C" void port_settings_game_boundary(int safe)'),
]
(out / "settings_save_runtime_actual.inc").write_text("\n".join(parts) + "\n")
(out / "settings_save_readiness_actual.inc").write_text(
    block(sequence, "bool SaveDataHandleSequence::isPreparedReset() const")
    + "\nnamespace GameSystemFunction {\n"
    + block(functions, "bool isPrepareResetSaveDataHandleSequence()") + "\n}\n"
)
(out / "settings_save_nand_actual.inc").write_text("\n".join([
    block(nand, "s32 NANDWrite("), block(nand, "s32 NANDClose("),
    block(manager, "void* NANDManagerThread::run()"),
    block(manager, "void NANDManagerThread::executeWriteSequence("),
]) + "\n")
