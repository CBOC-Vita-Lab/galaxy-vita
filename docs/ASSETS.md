# Included Files

## In The VPK

- The game executable, built from this repository.
- The compiled-shader seed: the port's own generated shaders, compiled for the
  vitaGL in use. No game data.
- The app icon and LiveArea pictures in `vita/vgx-game/sce_sys/`: simple
  original drawings made by `tools/handheld/make_livearea.py`. No retail
  artwork or screenshot.
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

The loading screen can show a picture of the game's title screen, but only if a
builder supplies it locally (`vita/vgx-game/sce_sys/loading-title.rgba`); it is
never in this repository or the release.
