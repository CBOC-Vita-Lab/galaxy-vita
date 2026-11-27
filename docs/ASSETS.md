# Included Files

## C182 Test Package

C182 uses the five contributor-supplied PNGs for its bubble, background, startup
gate, splash and Settings tile. See [artwork provenance](../vita/vgx-game/sce_sys/ARTWORK.md).
These pictures are contributor artwork that includes the game's key art and logo
(pic0 and the LiveArea background show the Super Mario Galaxy box illustration,
pic0 with the game's logo). They ship in the package and the public source as
supplied, by the owner's decision (17 Nov); the Settings tile picture is no longer
put in the package (3.0 has no Settings tile).
The automatic title capture remains available for in-game screens and does not
replace the installed art. Replacing it with generated pictures is a separate
legacy opt-in (`GXV_TITLEART_REPLACE_SHELL=1`). C182 also stores the inherited
loose shader binaries in one indexed package file. The list below describes the
whole package; its artwork paragraph is the same rule as this section.

## What the VPK holds

- The game executable, built from this repository.
- The compiled-shader seed: the port's own generated shaders, compiled for the
  vitaGL in use. No game data.
- The app icon and LiveArea pictures in `vita/vgx-game/sce_sys/` (icon0, pic0,
  bg, startup and the Settings tile): **contributor artwork**, five PNGs supplied
  by the project owner on 17 Nov 2026, whose contributor permits their free use
  in this game. They **include the game's key art** (the Super Mario Galaxy box
  illustration in pic0.png and livearea/contents/bg.png) **and its logo**
  (pic0.png). They ship in the package and the public source as supplied, **by the
  owner's decision**. They are never replaced or regenerated: a build must not
  draw new pictures over them, and `tools/handheld/make_livearea.py` (the earlier
  generated deep-space art) is not part of the release path. See
  `vita/vgx-game/sce_sys/ARTWORK.md` for the blob ids, the archive hash and the
  limits of the permission (it is not a general license to relicense the
  characters, marks or other underlying rights).
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

The bubble and LiveArea pictures in the package are the contributor artwork above, which
includes the game's key art and logo. Apart from those pictures the package carries no
Nintendo artwork. Until the game has run once, the loading screen is a dark starfield of
our own drawing with the progress bar and the Settings menu has its plain gradient background.

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
- **the bubble and LiveArea pictures**, written as 8-bit palette PNGs (256-entry
  palette, no transparency, no interlace) to `ux0:app/<title id>/sce_sys/` and to
  the console's own copy in `ur0:appmeta/<title id>/` (`icon0.png`, `pic0.png`,
  and the two LiveArea pictures when the install made that folder;
  `GXV_TITLEART_LIVEAREA=0` keeps those to the application folder):
  - `pic0.png` (960x544): the capture with its two text lines taken out (each
    hidden pixel interpolated from the sky around it, then the sky's own stars
    given back);
  - `livearea/contents/bg.png` (840x500): the same, cut to size, softened and
    darkened where the system draws its start gate and, towards the bottom, its
    frames, so that they stay legible;
  - `livearea/contents/startup.png` (280x158): the logo on the title's own sky,
    filling the gate picture within its margins;
  - `icon0.png` (128x128): the logo on its sky, scaled so that every logo pixel
    lies inside the round bubble with a margin (nothing of it is cut by the
    circle), and darker towards the rim so that the bubble reads on the home
    screen.

  Scaling is an antialiased cubic filter and the colours are reduced by a median
  cut with error diffusion, all in integer arithmetic. The title id is read from
  the running application (`SMGV00001` if that fails). The LiveArea "Settings"
  tile stays the package's own neutral picture.

The console's home screen keeps its own cache of the bubble pictures, which an
application cannot refresh. The new bubble shows once the home screen has read
the files again, which is after a restart of the console (or the console's own
"Refresh LiveArea" / database rebuild); the LiveArea page is read when it is
opened again. For the release notes: *the home-screen bubble and LiveArea made
from your game appear after the first time the title screen has been shown and
the console has then been restarted once; after installing an update, start the
game once and restart the console again.*

The capture is made once. It is made again when you delete
`ux0:data/smg/art/title.rgba`, or set `GXV_TITLEART=2` in `ux0:data/smg/env.txt`
(a new capture at every start); `GXV_TITLEART=0` turns the whole thing off. If a
package update (or a console refresh) puts the neutral pictures back, the game
notices at the next start (it compares the size and a checksum of the icon on
the card with what it wrote, recorded in `ux0:data/smg/art/title.ver`) and makes
the bubble pictures again from the saved capture, a few seconds after the title
screen appears, without capturing. When the pictures themselves improve (as in
this release), they are remade the same way from the saved capture. A
console that does not let the game write to the application folder or to
`ur0:appmeta` simply keeps the neutral bubble pictures; the loading screen and the
Settings backdrop do not depend on it. Every failure is silent apart from one line
in `log.txt`.

**As shipped, the console refuses those writes.** The game is installed as an ordinary
("safe") application, and the system does not let such an application change its own
folder or `ur0:appmeta`; the log says so once ("title art: the console does not let the
game change its home-screen pictures"). The game stays that way on purpose: asking for
extended permissions would give it the whole memory card. The bubble and LiveArea
pictures made from your title screen are therefore placed by the CBOC Vita Installer,
which reads the same capture (`ux0:data/smg/art/title.rgba`) over its FTP link when the
Vita connects and writes the four pictures with the same picture maker. With a VPK
installed by hand, the package's pictures (the contributor artwork above) stay. The loading screen and the
Settings backdrop are made by the game itself and do not depend on this.

`tools/handheld/make_livearea_title.py` is the host-side reference of this
algorithm: the same steps in integers with the same named constants, so that
for the same capture it gives the very same pixels as the console (checked on
made-up captures by `tests/test_title_art.py`, and on a real capture of your
own with `TITLEART_CAPTURE=<file>`). Use it to preview, or to tune the
geometry constants (`kTextY0`.., `kIconLogoRadius`, `kGateX0`..) from a real
capture. The code that runs on the console is
`platform/include/port/title_art.h`, `title_art_png.h` and
`vita/vgx-game/title_art_runtime.cpp`, tested by `tools/test_title_art.sh`.

## Making The Package Pictures

**The package pictures are the contributor artwork in `vita/vgx-game/sce_sys/`; never run
this into that folder and never regenerate them in a build** (`ARTWORK.md` there).
The maker below is kept only as a host tool for previews and as the reference of
the earlier generated art: `python3 tools/handheld/make_livearea.py [out-dir] [--style orbit|orbit2|spiral|comet]`
draws five such pictures into a folder you name (`orbit` is the default). It needs only NumPy
and gives the same bytes on every machine: all pixels are integer arithmetic,
the text is set from fixed letter outlines by its own rasteriser, and the
palette reduction and PNG compression are the project's own
(`tools/handheld/palette_png.py`; zlib and zlib-ng compress differently).
`tools/handheld/check_livearea.py` checks the pictures and the LiveArea template
against what the console installs, and `tools/handheld/livearea_preview.py`
draws contact sheets (all five pictures, the round bubble at 128 and 64 px, a
LiveArea mock) for choosing a style.

The pictures made on your console come from your copy of the game and remain
Nintendo's artwork; they stay on your card and are not part of this project or its
package, and the code licenses grant no rights to them.
