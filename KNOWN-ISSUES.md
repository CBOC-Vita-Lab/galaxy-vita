# Known Issues

Thanks for playing! If something here bothers you, or you find something new, open an [issue on GitHub](https://github.com/CBOC-Vita-Lab/galaxy-vita/issues).

## Performance
- **The Comet Observatory and the biggest boss fights are the heaviest scenes.** They can run below the rest of the game. The picture steps down a little there on its own.
- **A galaxy's first visit starts at standard sharpness.** It moves up at your first pause if the Vita has room, and remembers that afterwards.
- **The first launch is slower** while the graphics are prepared. Later launches are quicker.

## Gameplay
- **Rear touch is off by default**, so fingers holding the Vita never move the pointer by accident. Turn it on in the pause menu with Triangle.
- **A brief pause the first time some effects appear.** The Vita prepares each new graphics effect once, which can take a fraction of a second. The package comes with the effects of the whole game prepared ahead, and anything new is kept, so it happens at most once.

## Install
- The game needs your own `RMGE01` (Wii, USA) disc image. Other regions aren't supported yet.
- `libshacccg.suprx` must be set up on the Vita once (the installer and [Install](INSTALL.md) explain how).
- **Preparing the game by hand needs Python 3** on your computer, and shrinking the movies by hand needs a C++ compiler; full-size movies play less smoothly, so follow step 3 of [Prepare Your Game](PREPARE-GAME.md). The installer does both for you.

## If Something Goes Wrong
- **Something looks or runs wrong:** the game keeps a short log in `ux0:data/smg/log.txt` (and the session before it in `log.txt.prev`): the frame rate and any stutters every few seconds, picture changes, settings and errors. Attach it to your [issue](https://github.com/CBOC-Vita-Lab/galaxy-vita/issues).
- **The picture keeps changing sharpness, or you want it fixed:** add `GXV_AUTOQ=0` to `ux0:data/smg/env.txt` to keep the standard picture.
- **The HD character textures don't look right:** delete `ux0:data/smg/hdtex.pack`, or add `GXV_HDTEX=0` to the same file, to use the originals.
