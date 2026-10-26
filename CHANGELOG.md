# Version 1.1 — October 26, 2026

Mario's eyes, a Settings menu, and a steadier start.

### New
- **Settings menu.** Tap **Settings** on the game's LiveArea screen, before launching or while playing. It is drawn in the game's own fonts. Choose:
  - **Controls.** Remap every button for the Vita or for a PlayStation controller on a PS TV, with a picture of the controller.
  - **Picture rate.** Up to 60 FPS, or a fixed 30.
  - **Dynamic quality** and **reduced motion.**
  - **Game language,** from the languages your game files include.
  
  Choices are saved in `ux0:data/smg/player-settings.cfg`. Some apply after a restart, which the menu offers. Your saves are never touched.
- **Title-screen loading picture,** with a loading bar that shows real progress.

### Fixed
- **Mario's eyes.** Players with the optional HD textures saw his eyes white, or his eyelids closed all the time. Eyes now keep the game's own pictures, so they show and blink normally. The rest of the HD pack still applies.
- **Blue eyes without the HD pack too.** A colour calculation that the Wii wraps around was being clipped, which could leave his irises white. It now matches the Wii.
- **Start-up crash guarded.** A rare crash in the first seconds of a launch came from a file seek on a handle that was not open. That seek now fails safely.

### Easier to help with
- **A slow start tells you.** If loading takes more than 90 seconds, the screen says so and points you to the game files folder.
- **A fuller log.** `log.txt` (in `ux0:data/smg`) records each loading stage and when the title was reached. Each 5-second line shows how many shaders were prepared.

Updating? Install the new VPK over the old one. Your saves are kept. The first play after updating prepares the game's shaders again, so expect a few brief pauses the first time you visit each area.

# Version 1.0 — October 2026

Super Mario Galaxy, running natively on the PS Vita.

### Measured on a PS Vita
- **51 fps** on average in Good Egg Galaxy, **43 fps** in the Comet Observatory (the busiest place in the game), a full 60 in quieter moments.
- **0** stutters longer than a tenth of a second while playing, and **0** picture-size changes in the middle of play, in every test run.

### Touch controls and gameplay
- **Rear touch Star Bit aiming.** Slide on the rear pad to move the Star Bit pointer, like a trackpad, with your thumbs still on the sticks. Resting fingers are ignored. Turn it on in the pause menu with Triangle.
- **Settings panel in the pause menu,** drawn in the game's own fonts and window style: camera zoom with L and R (1.0× to 2.0×, 1.5× by default) and rear touch aiming with Triangle, with the game's own menu sounds.
- **Touch everywhere you used to point.** Tap to collect and choose, and hold still to grab Pull Stars. On menus, the left stick and Cross work too.
- **Tilt steering** for the Star Ball and the Manta ray, levelled to how you hold the Vita. Select resets it.
- **Vita button prompts** throughout the game's text and pictures.

### Smoother play
- **No resolution hitches.** The picture saves its sharpness changes for pauses and screen fades, and each galaxy remembers what works best for it.
- **Three cores at work.** The game, the graphics preparation and the GPU feed each run on their own Vita core, so busy scenes like the Comet Observatory keep moving.
- **Faster graphics engine.** Repeated graphics state is remembered instead of re-read every frame, and big scenes need fewer GPU passes.
- **Effects ready ahead of time.** 1,131 graphics effects ship already compiled for the Vita, so new ones load instantly instead of pausing the game.

### Better picture
- **Sharper than the Wii.** The game renders at up to the Vita's full 960×544 when there's room, with anti-aliased edges on top, and a sharpened upscale when it renders smaller.
- **Optional HD characters and items.** Sharper textures for Mario, Luigi, Peach, Rosalina, the Lumas, Toads, Bowser, Bowser Jr., the Power Stars, launch stars and power-ups, made on your computer from your own game files. [How](tools/hdtex/README.md).
- **Full-screen 16:9** by default.

### Under the hood
- Shares its graphics engine work with our Mario Strikers Charged port: safe texture clean-up while a scene is drawing, a large shader-program cache, and careful video memory use.
- A short log in `ux0:data/smg/log.txt` (frame rate, stutters, freezes, picture changes, errors) to attach to bug reports.

The full engineering list is in [docs/TECHNICAL.md](docs/TECHNICAL.md#whats-in-version-10).

