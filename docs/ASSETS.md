# Included Files

## In The VPK

- The game executable, built from this repository.
- The compiled-shader seed: the port's own generated shaders, compiled for the
  vitaGL in use. No game data.
- The app icon and LiveArea pictures in `vita/vgx-game/sce_sys/` (icon0, pic0,
  bg, startup and the Settings tile): original drawings (a starfield, a
  five-pointed star and plain text) made by `tools/handheld/make_livearea.py`.
  **No game artwork is in the package**: no title-screen picture, no logo, no
  capture. The game makes its own pictures from your copy, see below.
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

## Pictures Made From Your Own Copy

The package carries no Nintendo artwork. Until the game has run once, the loading
screen is a dark starfield of our own drawing with the progress bar, the Settings
menu has its plain gradient background, and the home-screen bubble and LiveArea
show the neutral package pictures.

The first time the real title screen has been shown completely (the logo fully
in, about two seconds after the loading screen has gone and the picture has
stopped changing), the game copies the 960x544 picture on the screen, once, to
`ux0:data/smg/art/title.rgba` on your own card. A low-priority background thread
does this, never the game's or the renderer's. Nothing is uploaded and nothing
leaves the console. From that one capture the same thread makes:

- **the loading-screen picture**, used from the next start on (the progress bar
  and text replace the "Press START" line, as before);
- **the Settings menu backdrop** (darkened and halved in memory while the menu is
  open);
- **the bubble and LiveArea pictures**: the capture with its two text lines taken
  out, as `pic0.png` (960x544) and `livearea/contents/bg.png` (840x500), and the
  logo on its sky as `livearea/contents/startup.png` (280x158) and `icon0.png`
  (128x128), written as 8-bit palette PNGs to
  `ux0:app/<title id>/sce_sys/` and, for `icon0.png` and `pic0.png`, to the
  console's own copy in `ur0:appmeta/<title id>/`. The title id is read from the
  running application (`SMGV00001` if that fails). The LiveArea "Settings" tile
  stays the package's own neutral picture.

The console's home screen keeps its own cache of the bubble pictures. The new
bubble shows once the home screen has read them again, which is after a restart
of the console (or any time it refreshes the application's files); the LiveArea
page (its background and start picture) is read when the page is opened. Nothing
is needed from the player.

The capture is made once. It is made again when you delete
`ux0:data/smg/art/title.rgba`, or set `GXV_TITLEART=2` in `ux0:data/smg/env.txt`
(a new capture at every start); `GXV_TITLEART=0` turns the whole thing off. If a
package update puts the neutral pictures back, the game notices at the next start
and makes the bubble pictures again from the saved capture, without capturing. A
console that does not let the game write to the application folder or to
`ur0:appmeta` simply keeps the neutral bubble pictures; the loading screen and the
Settings backdrop do not depend on it. Every failure is silent apart from one line
in `log.txt`.

`tools/handheld/make_livearea_title.py` is the host-side reference of this
algorithm (the same steps with OpenCV and Pillow, for comparison and for
previews); the code that runs on the console is
`platform/include/port/title_art.h`, `title_art_png.h` and
`vita/vgx-game/title_art_runtime.cpp`, tested by `tools/test_title_art.sh`.

The pictures made on your console come from your copy of the game and remain
Nintendo's artwork; they stay on your card and are not part of this project or its
package, and the code licenses grant no rights to them.
