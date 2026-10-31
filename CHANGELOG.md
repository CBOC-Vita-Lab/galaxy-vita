# Version 1.2 — November 2026

The smooth-play update: aiming for a steady 30 with the full picture.

### Smoother play
- **Back to 1.0's speed, and past it.** 1.1 made every player rebuild their shaders during play and added work to every frame. Both are undone: your existing shader cache is used again, and the package brings the extra shaders 1.2 needs already prepared.
- **30 FPS is the floor.** In every picture rate, dynamic quality watches the whole frame's work and lowers resolution only when a scene would otherwise fall under 30, then brings it back within seconds.
- **Lock it at 30.** Settings > Picture rate now offers **Locked 30** (an even 30 FPS, every frame, at full effects) or **Up to 60** (the default). Locked 30 replaces 1.1's "30 FPS" and "Steady 30", and it never drops to 20.
- **The game and the renderer work side by side again.** Under a 30 FPS cap they had started taking turns, so each frame cost both of them added together. This was the main cause of the slow Comet Observatory.
- **No more drops to 15 FPS.** When a scene is just too busy for the processor, the game now keeps showing every picture and slows down a touch, as the Wii did, instead of halving the frame rate.
- **Less work every frame.** Matching pieces of scenery are drawn together, lights are worked out once instead of twice, screen copies use their own small programs, and sound mixing has moved off the game's own processor core.
- **Movies on the Vita's video hardware.** The title video and cutscenes are decoded by the console's JPEG hardware, with the software decoder as a fallback.
- **The newest vitaGL**, with its speed improvements.

### Your own pictures
- **No game artwork in the package.** The first time you reach the title screen, the game saves that picture on your card (`ux0:data/smg/art`) and makes the loading screen, the Settings background and the home-screen bubble and LiveArea pictures from it. Until then the loading screen is a plain starfield. The new bubble shows after the console's home screen refreshes (a restart). `GXV_TITLEART=0` turns it off.

### Settings
- **A fuller Settings menu.** The Display page now has a **Preset** (Best picture, Balanced, Performance; any covered row you change reads Custom), **Dynamic resolution**, **Sharper when possible**, **Prioritize Mario**, **Anti-aliasing**, **Sharpness** (Auto, Off, Strong), **HD textures** (it says when the pack is not installed), **Effects quality**, **Depth of field**, **Core plan**, Reduced motion and Game language, each with a two-line explanation at the bottom and a button legend. The Controls page shows a table of what every button does in the current layout, and the moves that use them. The heading is drawn in the game's own menu font, over the title picture.
- **Tilt, your way.** Settings > Controls has **Tilt sensitivity** (50% to 200%, 100% is the game's own feel) and **Tilt with** (Motion sensor or Left stick; a console without a motion sensor always uses the stick) for Star Ball and ray surfing, applied from the next launch.

### Better picture
- **Bowser no longer twists and spins** during his battle introductions (also fixed: the Bowser Jr. ending, three Polta scenes and a Dino Piranha scene).
- **Matter Splatter's ground** no longer flashes black, and leftover shine, light and shadow darkening are gone.
- **Ground-pound and black-hole trails are back,** along with the right colours on about 85 other effects (Dino Piranha, Mario, Dodoryu, Kingfin and more) and a few shadows and bubbles. Colours written in the game's effect list were being read with their bytes in the wrong order.
- **Mario's eyes in HD.** With the optional HD textures, Mario's eyes now use the sharper HD images too. The white or stuck-closed eyes that made 1.1 leave them out came from a colour rule the port now follows exactly.
- **Sparkles and rain lines at full size.** Thin-line and dot effects (the spin sparkles, rain, depth-of-field strips) were drawn one pixel wide; they now have the Wii's widths.
- **Lumas have their colours back.** They could show up as white blobs; a Wii colour rule the port missed is now followed.
- **Queen Bee's fur** looks as it should.
- **Textures as sharp or soft as the game asks for,** including the dome floors and the Dark Matter Plant, and water keeps the game's own distant detail.
- **Trails, bump and water effects** follow the Wii's rules more closely.
- **Reduced motion works in play.** Settings > Display > Reduced motion now cuts camera shake to a quarter of its size and the screen blur on explosions and the first-person camera to half, and the Star Pointer still hits what you see. Nothing else about how the game plays changes.

### Controls and gameplay
- **Music keeps playing after sleep or the Home menu.**
- **Dialogue no longer skips to the end** when a finger rests on the screen.
- **Spins answer at once.** A Square or R spin right after a jump used to come a third of a second late (or not when expected); it now spins the moment the game allows, and quick taps always count.
- **PS TV with a PS3 or PS4 controller:** R2 shoots Star Bits, the right stick moves the pointer, and the D-pad keeps the camera.
- **Mouse-style aiming** for the Star Bit pointer, as an option in Settings > Controls.

### Steadier start
- **The loading screen can't hang at 99%.** If the game is running but the title isn't recognised, the title picture fades out by itself. If loading truly stalls, the screen says what it is waiting for.
- **Clearer graphics errors.** A missing or broken shader compiler, or a graphics start-up failure, now names the exact step.
- **Safer graphics driver.** Running out of graphics memory skips a frame instead of freezing the Vita.

Updating? Install the new VPK over the old one. Your saves and settings are kept.

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

