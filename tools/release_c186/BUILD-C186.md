# Building the optional C186.2 hitch diagnostic without a console

C186.2 changes hitch reporting, not presentation or frame pacing. In the
C186 emulator log the two 126/130 ms intervals included 109/104 ms in the
present path. The old classifier ignored that field. Slow-frame logs now show
the existing measured present phase and the render thread's GXM display queue
calls for the whole frame. No sampled queue calls means unavailable, not a
measured zero. The queue duration must not be added to the present phase.
No new clock reads are needed. This is not a measured Vita FPS or hitch
improvement; players need not replace C186.1 for these diagnostics. Host-side
emulator configuration changes are separate from the VPK.

C186.1 backports the native Settings link correction documented by Claude in
`reports/linkfix31.md` (fix `c9919deb`, checksums `048013bc`). This is the
scoped backport, not a merge of the later 3.1 integration. The original C186
ELF left all seven live Settings hooks undefined weak, so Triangle at pause
could not open Settings. The current release requires those hooks, the menu
request, and the pause hint to be strong global text in the final ELF.
`python3 -B tests/test_c186_settings_link.py` checks both sides of the gate
with real ARM objects; a physical pause-menu/restart test is still required.
(On integ-3.1, c186merge: the nine-name list and its checker are folded into
the general post-link check, `tools/check_weak_links.py --must-define
tools/vita_must_define.txt`, which now also requires a global definition;
the test exercises that check. The C186.2 commit itself used the C186 list.)
The executable boot ID and VPK filename distinguish C186.2. The bubble title
ID and 03.00 version stay unchanged to preserve the install and saves.

Check out the bundled C186.2 commit on top of the exact integration commit
`e44c45d8bed8f089d3dc75eb2b4b3091bbb1c491`. From that checkout run:

```sh
bash tools/release_c186/rebuild-local.sh
```

The launcher expects the locally pinned `codex-galaxy-native:20261007` compiler
image (Linux AMD64) and build inputs. It does not pull a container, fetch
dependencies, mount the device, install, upload, or deploy. Game disc data,
saves, keys, and device shader captures are not inputs. It uses the committed
git tree, not untracked files. The tools reject a mismatched compiler, SDK,
dependency/TLS source archives, release PGO kit or donor VPK. The private SDK
and original release prerequisites are not included in the source bundle.

Defaults assume the original workspace layout. For a fresh checkout the
following variables can point to your copies:

- `C186_KIT`: path to `c181-build-kit-1115.tgz` containing the c181 PGO profile and vitaGL reader.
- `C186_TLS_INPUTS`: directory with `mbedtls-068ff080-checkout.tgz`, `vdeps.tgz`, and `vitasdk.tgz`.
- `C186_DONOR`: path to `Galaxy-Vita-2.3-test19.vpk` for the **unaltered** seed files (no game disc).
- `C186_OUT`: output directory (default `../outputs/galaxy-c186.2` relative to the checkout).
- `C186_TREE`: checkout path when using a copy of the scripts in the output directory.

Use an empty output directory to compare a second build without changing the
original. The final build pins `SOURCE_DATE_EPOCH` to the integration commit,
uses the same internal source/build paths, and writes ZIP members in sorted
order with fixed archive metadata. For byte-for-byte ZIP comparison use the
same host Python/zlib as the package receipt. The linker checks actual ELF
features and TLS/network symbols; the packager independently verifies the
SELF executable payload, integration template and SFO, title ID, memory
budget, contributor PNGs, all donor shader records, and CRCs. The fifth
contributor settings PNG is included as an inert asset, without reintroducing
the removed LiveArea settings tile.
