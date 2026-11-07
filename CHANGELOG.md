# Version 2.1 — November 7, 2026

The smooth-play update. Far fewer pauses while the game prepares new graphics.

### Smoother play
- **Many more graphics ready out of the box.** The download now carries ready-made shaders gathered from every mission of every galaxy, not just the first, so the game almost never has to build a shader mid-play anymore. In our test of eight galaxies on a fresh install, only 4 shaders had to be built on the Vita, and those are in the download too. A short hitch can still appear in the first moments of a galaxy while its graphics are linked.
- **Galaxies warm up while they load.** Each galaxy prepares the graphics it's known to need during its loading screen, adding at most a second or so, so your first steps in a galaxy are as smooth as the rest.
- **The list of what each galaxy needs now ships with the game,** so even a fresh install gets that warm-up on its very first visit.

### Under the hood
- An optional background shader compiler is included for testing (`GXV_ASYNC_SHADERS=1` in `env.txt`). It's off by default while it's being tuned.
- The package no longer carries a stray macOS file.

**Updating?** Install the new VPK over the old one. Your saves and settings stay.

# Version 2.0 — November 7, 2026

The full-resolution update. Super Mario Galaxy now runs at the Vita's native 960×544 at a steady 30 FPS, with a new Performance mode that reaches for 60.

### Pick your picture
- **Quality (the default):** the sharpest picture at a steady 30 FPS, at up to the full 960×544. In the busiest moments it softens edges first, then sharpness a step at a time, and brings everything back as soon as there's room.
- **Performance:** as close to 60 FPS as each scene allows. When 60 isn't possible it settles on a smooth 30 rather than a choppy 40.
- **Locked 30:** always 960×544, always 30 FPS when the Vita can manage it.

### Smoother play
- **Honeyhive, Beach Bowl and the Bowser fights hold 30 FPS** in Quality mode. They used to drop into the teens and 20s.
- **All four CPU cores.** With the CapUnlocker plug-in, sound, animation and part of the graphics work move to the Vita's fourth core.
- **The game and the graphics work side by side,** so the game never waits on the renderer between frames.
- **Leaner graphics:** lighter shaders, cheaper water and glow effects, and nothing drawn that ends up off screen.
- **Even frame pacing,** with each frame shown right on time.

### Better picture
- **Widescreen picture sizes** and texture sharpness that follows the resolution.
- **Edge smoothing** across the whole picture, with the HUD kept crisp.
- **No more stray lines:** a 1-pixel line in Beach Bowl and under menu buttons, and a line under the star counter, are gone.
- **The unpowered Comet Observatory,** early in a new game, now has dim but visible floors.

### Controls and gameplay
- **New pause-menu options:** use the right stick as the Star Bit pointer (Square), and set Star Bit reach (Select) to collect from farther away.
- **Enemies that grab and release Mario hurt him again,** just like on the Wii.
- **Hidden-star missions** work properly.
- **Rear touch works right after the Settings menu.**
- **Spins come out every time,** even when you press Square right after a jump.

### Under the hood
- Shaders are prepared again after an update, graphics memory stays tidy in long sessions, and a 500 MHz PSVshell profile is left alone.
- The HD texture pack builder matches the game's colours exactly.
- Full details are in [Technical notes](docs/TECHNICAL.md) and [Source record](docs/SOURCE.md).

### Thanks
Several fixes and ideas come from Producdevity's fork of version 1.1: Star Bit reach, the right-stick pointer, the enemy-damage, hidden-star and rear-touch fixes, and some speed-ups. See [Credits](CREDITS.md).

**Updating?** Install the new VPK over the old one. Your saves and settings stay. Measured frame rates are in [Performance](docs/PERFORMANCE.md).

# Version 1.2 — October 31, 2026

The picture-perfect update: a sharper, correct picture, a 30 FPS floor in most of the game, and a real Settings menu.

### Better picture
- **Bowser no longer twists and spins** in his battle introductions (also fixed: the Bowser Jr. ending, three Polta scenes and a Dino Piranha scene).
- **Lumas have their colours back** instead of showing up as white blobs.
- **Ground-pound and black-hole trails are back,** with the right colours on about 85 other effects.
- **Matter Splatter's walkways appear as they should** when the light reaches them, instead of flashing black.
- **Mario's eyes in HD** with the optional HD textures, and **Queen Bee's fur** looks right.
- **Sparkles and rain at full size,** and textures as sharp or soft as the game asks for.
- **A sharper picture by default.** Most galaxies now stay at 640×456 or better, where 1.0 often stepped down.

### Smoother play
- **A 30 FPS floor.** The quality system now puts 30 FPS first: in a heavy scene it lowers the sharpness and the effects that matter least, keeps Mario at full quality, and brings everything back within seconds when there's room.
- **Locked 30.** Prefer an even, steady picture? Settings > Picture rate > **Locked 30** holds a smooth 30 every frame.
- **No first-visit freezes.** Every galaxy's graphics come ready-made in the package, so entering a galaxy for the first time no longer stops the game for seconds.
- **Much faster start.** The title screen comes up in about 20 seconds (1.0 took about 40), and the first launch after installing is just as quick.
- **Rock solid in long sessions.** A memory problem that could crash the game after many galaxies in one sitting is fixed (tested through all 45 galaxies in a row).
- **Movies on the Vita's video hardware,** and sound mixing on its own core.

### Settings
- **A full Settings menu,** from the game's LiveArea screen: picture presets, sharpness, anti-aliasing, effects, HD textures, depth of field, reduced motion, language, and a Controls page that shows what every button does. It's drawn in the game's own font, over your own title screen.
- **Tilt your way.** Tilt sensitivity on a simple 1–10 scale, and steer the Star Ball and Manta Ray with the left stick instead of tilting, if you prefer.
- **Mouse-style aiming** for the Star Bit pointer, as an option.
- **Reduced motion** now really works in play: much less camera shake and blur.

### Controls and gameplay
- **Music keeps playing** after sleep or the Home menu.
- **Dialogue no longer skips to the end** when a finger rests on the screen.
- **Spins answer at once,** even right after a jump.
- **PS TV with a PS3 or PS4 controller:** R2 shoots Star Bits, the right stick moves the pointer, the D-pad keeps the camera.
- **Tidier hints.** Rewritten prompts for the Vita's controls wrap at whole words.

### Your own title screen
- **No game artwork in the download.** The first time you reach the title screen, the game saves it on your memory card and uses it for the loading screen and the Settings background. The CBOC Vita Installer builds the home-screen bubble from your own game; a manual install keeps a plain star (the console doesn't let a game change its own bubble).

### Steadier start
- **The loading screen can't hang at 99%,** and a real stall says what it's waiting for.
- **Clearer errors** if the shader compiler or the graphics fail to start, and running out of graphics memory skips a frame instead of freezing the Vita.

Updating? Install the new VPK over the old one. Your saves and settings are kept. The full engineering list is in [docs/SOURCE.md](docs/SOURCE.md#version-12).

# Version 1.1 — October 26, 2026

Mario's eyes, a Settings menu, and a steadier start.

### New
- **Settings menu.** Tap **Settings** on the game's LiveArea screen, before launching or while playing. It is drawn in the game's own fonts. Choose:
  - **Controls.** Remap every button for the Vita or for a PlayStation controller on a PS TV, with a picture of the controller.
  - **Picture rate.** Up to 60 FPS, a fixed 30, or **Steady 30**: an even 30 FPS that lowers the resolution and effects quality when needed. For players who prefer a steady picture to the highest frame rate.
  - **Dynamic quality** and **reduced motion.**
  - **Game language,** from the languages your game files include.
  
  Choices are saved in `ux0:data/smg/player-settings.cfg`. Some apply after a restart, which the menu offers. Your saves are never touched.
- **Title-screen loading picture,** with a loading bar that shows real progress.

### Fixed
- **Mario's eyes.** Players with the optional HD textures saw his eyes white, or his eyelids closed all the time. Eyes now keep the game's own pictures, so they show and blink normally. The rest of the HD pack still applies.
- **Blue eyes without the HD pack too.** A colour calculation that the Wii wraps around was being clipped, which could leave his irises white. It now matches the Wii.
- **Start-up crash guarded.** A rare crash in the first seconds of a launch came from a file seek on a handle that was not open. That seek now fails safely.

### Faster
- **Quicker sound mixing.** The game's audio is now mixed with the Vita's vector unit, which frees a little time on the game's busiest thread. The sound is bit-for-bit the same.

### Easier to help with
- **A slow start tells you.** If loading takes more than 90 seconds, the screen says so and points you to the game files folder.
- **A fuller log.** `log.txt` (in `ux0:data/smg`) records each loading stage and when the title was reached. Each 5-second line shows how many shaders were prepared.

Updating? Install the new VPK over the old one. Your saves are kept. The first start after updating takes a little longer while the game prepares its updated shaders.

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

