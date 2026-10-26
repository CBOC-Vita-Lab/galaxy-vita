#!/usr/bin/env python3
"""Compile the actual queue/ownership adapters with only GPU/FIFO boundaries faked."""
import pathlib
import sys

source = pathlib.Path("platform/src/gx/vgx_galaxy.cpp").read_text()
blocks = []
for name in ("void ServiceSettingsHold(", 'extern "C" void port_vgx_settings_release(', 'extern "C" int port_vgx_settings_hold('):
    start = source.index(name)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    blocks.append(source[start:end])
pathlib.Path(sys.argv[1]).write_text("\n".join(blocks) + "\n")
