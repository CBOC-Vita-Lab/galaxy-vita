#!/usr/bin/env python3
"""Extract actual driver-boundary hooks, not a second implementation."""
import pathlib
import sys
source = pathlib.Path("platform/vgx/vita/vgx_host_galaxy_vita.cpp").read_text()
blocks=[]
start=source.index('bool GamePresentAllowed(')
end=source.index('{',start)+1
depth=1
while depth:
    depth += (source[end]=='{')-(source[end]=='}')
    end+=1
blocks.append(source[start:end])
for signature in ('void port_vgx_vita_reset_present_slots(', 'int port_vgx_vita_fixed_cap_supported(',
                  'void port_vgx_vita_set_present_allowed(', 'void port_vgx_vita_finish_unpresented_frame(',
                  'unsigned long long port_vgx_vita_game_swaps(', 'int port_vgx_vita_settings_finish(',
                  'void port_vgx_vita_settings_resume('):
    start=source.index('extern "C" '+signature)
    end=source.index('{',start)+1
    depth=1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}')
        end+=1
    blocks.append(source[start:end])
pathlib.Path(sys.argv[1]).write_text('\n'.join(blocks)+'\n')
