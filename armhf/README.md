# armhf harness (32-bit ARM Linux build of the game, run under qemu-arm)

The game units with the PS Vita build's flags and heap sizes (`-D__vita__`, `PORT_ARMHF_HARNESS`), the platform layer, vgx with a
null back end, and `net/` (POSIX UDP + host keyboard stand-ins), linked with the GNU cross driver. It is the closest thing to a
Vita that runs on a PC; it is for crash/alignment/size checks and instruction counts, not for playing.

## Build
    # needs clang, g++-arm-linux-gnueabihf, qemu-user, cmake, ninja
    # CPATH: the decomp includes headers with the wrong case (JASDSPInterface.hpp vs the file JASDspInterface.hpp, from a
    # case-insensitive Mac); on Linux the desktop case shim resolves them (tools/cc.sh does the same).
    export CPATH=/home/claude/g3/desk/shim
    cmake -S armhf -B build-armhf -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/armhf/toolchain.cmake -DCMAKE_BUILD_TYPE=
    nice -n 10 ninja -C build-armhf -j2 game        # ~1930 units; about 2 h on the shared 2-core box

## Run
    qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf build-armhf/game <cooked-disc> <run-dir> <seconds>
Same arguments and PETARI_* environment as the desktop `game` (platform/desktop/main.cpp). Emulation is slow (about 50x): give the
run plenty of wall time and set PETARI_STALL_S high (120+). On the shared box wrap runs in `tools/gamerun.sh` (one run at a time).

Multiplayer smoke (two processes over 127.0.0.1): use tools/mp/harness/run_pair.sh with `HARNESS_BIN` pointing at a wrapper script
that execs `qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf build-armhf/game "$@"`, and `-P host=SMG_MP_AUTO=host -P guest=SMG_MP_AUTO=join`.

## Build notes (found 8 Nov 2026)
- Builds go through tools/buildrun.sh on the shared box. net/ sources are listed in armhf/CMakeLists.txt (`smg_net`): when the
  desktop `smg_net` list in the root CMakeLists.txt gains a file (np_upnp.cpp did), add it there too, or the link fails on NpNet::*.
- par_exec.h / par_spec.h use psp2 kernel headers under `__vita__`; they are guarded with `!defined(PORT_ARMHF_HARNESS)` (Linux threads).
- The harness always uses the rebased memory layout (MEM1 0x60000000, MEM2 0x70000000): the image sits at 0x80000000.

## Smoke results (mp-int of 8 Nov 2026)
Single process: boots to the title screen ("boot: the title screen has appeared") under qemu-arm. Use `PETARI_SHOT_MS=100000000`:
with the 2 s default, a run froze at a screenshot (port_vgx_capture) after ~2400 frames.
Two processes (run_pair.sh, SMG_MP_AUTO=host/join, VLink over 127.0.0.1): scan, join, session start, 8052-byte progress snapshot
(same CRC on both sides), barrier, puppets on both sides, no bad messages. About 90 s of guest time takes ~400 s of wall time for the
pair, so give run_pair.sh a long timeout or a small -s.
