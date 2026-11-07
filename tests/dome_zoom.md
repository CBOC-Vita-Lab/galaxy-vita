# Dome zoom and pointer checks

Run `bash tools/test_dome_zoom.sh` on a host with Python 3 and Clang. It extracts
the production zoom decision, projection and StarPointerTarget methods, and runs
them with ASan/UBSan. No game assets, save files, device or native game build are
needed. Zoom settings reads use an in-memory file boundary, never a filesystem.

The opt-in `GXV_ASTRO_DOME_ZOOM_GUARD=1` keeps the original field of view in
`AstroDome`, whose fixed room cameras can place the Pull Star near the upper
edge. A zoom can move its center offscreen even while part of its model remains
visible. The game's pointer target rejects offscreen centers before checking
radius overlap. The guard does not modify that target rule, the actor, input,
saved zoom or any other stage's gameplay zoom. It is default off and read once.

Coverage includes every zoom step from 1.0 to 2.0, 4:3 and 16:9 projection,
default/OFF/ON/non-enabling flag values, read-once behavior, other gameplay,
entry/exit transitions, demos, event cameras and file select. A synthetic edge
target establishes the crop failure without distributing game data. Optional
arguments `viewX viewY viewZ fovy` repeat the test with externally obtained
view-space coordinates. `--expect-guard` is a RED check against unmodified source.

`VITASDK=/path/to/sdk bash tools/test_dome_pointer_input.sh` additionally compiles
the complete production Vita input and KPAD translation against SDK function
stubs. It checks 25 front-touch positions for each remapping, full-width/side-bar
and touch-A combination, plus Cross and auto-held A trigger edges. This is a host
adapter test; SDK sampling, actual on-device camera state, target depth/occlusion,
actor nerve transitions and rendered pixels still require independent validation.
