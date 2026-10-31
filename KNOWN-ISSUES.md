# Known Issues

Thanks for playing! If something here bothers you, or you find something new, open an [issue on GitHub](https://github.com/CBOC-Vita-Lab/galaxy-vita/issues).

## Performance
- **The heaviest scenes dip below 30.** Beach Bowl's water, Bonefin, the Bowser fights and Matter Splatter's dark matter are the hardest work for the Vita. The picture gets less sharp there on its own and usually holds about 30, but the busiest moments can dip into the 20s. **Locked 30** (Settings > Picture rate) keeps an even 30 where the Vita can.
- **The Comet Observatory runs at about 38 fps,** a little below 1.0's 42: the picture fixes cost a little there.
- **A 60 fps "Performance" mode** is being worked on for a later update.
- **With a manual install, the home-screen bubble is a plain star.** The console doesn't let a game change its own bubble pictures; the CBOC Vita Installer builds the bubble from your own game. The loading screen and Settings background use your own title screen (from your second start on). Delete `ux0:data/smg/art/title.rgba` to capture it again, or add `GXV_TITLEART=0` to `ux0:data/smg/env.txt` to switch it off.

## Gameplay
- **Rear touch is off by default**, so fingers holding the Vita never move the pointer by accident. Turn it on in the pause menu with Triangle.
- **A rare brief pause the first time an effect appears.** The package comes with the graphics of every galaxy prepared ahead; anything it doesn't have is prepared once (a fraction of a second) and kept.

## Install
- The game needs your own `RMGE01` (Wii, USA) disc image. Other regions aren't supported yet.
- `libshacccg.suprx` must be set up on the Vita once (the installer and [Install](MANUAL-INSTALL.md) explain how).
- **Preparing the game by hand needs Python 3** on your computer, and shrinking the movies by hand needs a C++ compiler; full-size movies play less smoothly, so follow step 3 of [Prepare Your Game](MANUAL-PREPARE-GAME.md). The installer does both for you.

## If Something Goes Wrong
- **Something looks or runs wrong:** the game keeps a short log in `ux0:data/smg/log.txt` (and the session before it in `log.txt.prev`): the frame rate and any stutters every few seconds, picture changes, settings and errors. Attach it to your [issue](https://github.com/CBOC-Vita-Lab/galaxy-vita/issues).
- **The picture keeps changing sharpness, or you want it fixed:** add `GXV_AUTOQ=0` to `ux0:data/smg/env.txt` to keep the standard picture.
- **The HD character textures don't look right:** delete `ux0:data/smg/hdtex.pack`, or add `GXV_HDTEX=0` to the same file, to use the originals.
