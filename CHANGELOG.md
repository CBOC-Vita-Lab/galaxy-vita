# Version 1.1 — October 25, 2026

A small fix for the HD textures.

### Fixed
- **Mario's eyes.** Some players with the optional HD textures saw Mario's eyes white, or his eyelids closed all the time. Eyes now always use the game's own pictures, so they show and blink normally. The rest of the HD pack still applies, and you don't need a new pack.

### Easier to help with
- **A slow start tells you.** If the loading screen is still going after 90 seconds, it says so and points you to the game files folder.
- **A fuller log.** `log.txt` (in `ux0:data/smg`) now records each loading stage and when the title was reached, and each 5-second line shows how many shaders were prepared, so a stutter or a stuck start is much quicker to diagnose. Attach it when you report a problem.

Updating? Install the new VPK over the old one. Your saves and settings are kept.

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

