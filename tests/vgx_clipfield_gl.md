# Clip-Field Renderer Probe

This synthetic test exercises the production GX front end and GLES backend with
no game assets. It is not a gameplay or physical-Vita acceptance test.

The sequence models `ClipArea` and `FallOutFieldDraw`: full floor geometry,
depth-tested alpha add/subtract volumes, A8 capture, TEV alpha comparison,
outside fill with `GXSetZScaleOffset(0, 1)`, and a blended edge that requests
constant destination alpha zero. The reveal mask and far-depth fill already
passed before the change. The edge's alpha clear did not: the legacy values
were 66/64 rather than 0/0. That defect is concrete, but its relationship to
the user's missing floor is unproven.

`GXV_DSTALPHA_ZERO=1` enables an experimental zero-only override, read once.
Unset, `0`, and values not parsed as `1` retain legacy behavior. The override applies only
when GX blending/subtract is enabled, EFB format has alpha, alpha updates are
enabled, and destination alpha is enabled with value zero. RGB blending and
fragment alpha/depth rejection are unchanged. Nonzero destination-alpha values
remain unsupported with blending. No shader text/key layout changes are made.

Run on a Linux host with EGL/GLES2 development libraries:

```sh
CXX=clang++ bash tools/test_vgx_clipfield_gl.sh /tmp/clipfield-results
```

The script runs four fresh processes for unset/0/invalid/1, checks read-once
behavior by flipping the environment after initialization, and exercises
ordinary blend restoration, RGB preservation, alpha/depth rejection, disabled
alpha updates, RGB8 alpha-copy behavior, nonzero legacy requests, reverse
subtract, and raw ONE/ZERO blending.

Additional coverage:

```sh
bash tools/test_vgx_variant_hash.sh
bash tools/test_vgx_shader_list.sh
bash tools/test_vgx_shader_list.sh arm32
bash tools/test_vgx_warm_variants.sh
```

Variant identity retains the previous 12-bit blend/layout packing when the
override is absent; override bit 12 occupies formerly unused packed bit 24.
Current readers accept both formats. Older builds do not understand new
override recipe records and may truncate/relearn a list containing them.
Use an isolated shader-list/cache location for an opt-in experiment.

The warm-variant host test provides test-local ample-VRAM/non-null texture
backing stubs for Vita-only allocator queries. All blending, shader patch
state, drawing, GL-state queries, and pixel readback use real Mesa. These stubs
are not evidence of Vita allocation or memory-pressure behavior.
