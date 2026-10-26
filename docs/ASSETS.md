# Included Files

## In The VPK

- The game executable, built from this repository.
- The compiled-shader seed: the port's own generated shaders, compiled for the
  vitaGL in use. No game data.
- The app icon and LiveArea pictures in `vita/vgx-game/sce_sys/`: simple
  original drawings made by `tools/handheld/make_livearea.py`. No retail
  artwork or screenshot in those generated pictures.
- New builds include the preserved title-screen loading background described
  below. It is not original port artwork and is separate from the generated
  icon and LiveArea pictures.
- License texts for the port and the libraries it uses.

[RELEASE.json](RELEASE.json) records the package and executable hashes.

## In The Install ZIP

The VPK, plus the guides, notices, checksums, the full source and the
computer-side tools: the game converter (`tools/cook/`), the optional HD
texture builder (`tools/hdtex/`) and the download checker
(`tools/check-files.py`, [MIT licensed](../licenses/CBOC-tools-MIT.txt)).

## Not Included

Your game (disc image, extracted or converted files, movies, music), the HD
texture pack made from it, saves, firmware and the Vita's shader compiler module
(`libshacccg.suprx`). Supply your own. The code licenses grant no rights to the
original game's assets.

## Preserved Loading Background

`vita/vgx-game/sce_sys/loading-title.rgba` is the selected Galaxy title-screen
background, retained unchanged as the explicitly requested exception to the
source tree's general exclusion of game captures. It is 960x544, 8-bit RGBA,
2,088,960 bytes, with SHA-256:

`14529d51dc9e3c4831a20f389b90c162f93ddeafc7567b206fe3b976dd679906`

These bytes match the preserved title-loader resource and historical installed
file backups. This is dated local provenance, not a current device readback.
The existing loading renderer replaces the start prompt with its progress bar
and status text; the logo, copyright notice and pixels outside that loading
band are unchanged. The source asset is not a newly generated imitation.

The title artwork and its Nintendo notice remain third-party material, not
covered by the port's code licenses. Including this particular image does not
grant rights to other game assets or make the underlying artwork original to
this project. No other game data, fonts, music, textures or captures are added.

The native packaging recipe checks this asset's exact size and hash and fails
instead of silently dropping it. Runtime fallback for a missing/unreadable
installed image remains unchanged. The frozen 1.0 VPK predates this source
change; no executable, release manifest or package hash has been regenerated.
