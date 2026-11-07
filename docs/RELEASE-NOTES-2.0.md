The full-resolution update. Super Mario Galaxy now runs at the Vita's native 960×544 at a steady 30 FPS, with a new Performance mode that reaches for 60.

### Pick your picture
- **Quality (the default):** the sharpest picture at a steady 30 FPS, at up to the full 960×544. In the busiest moments it softens edges first, then sharpness a step at a time, and brings everything back as soon as there's room.
- **Performance:** as close to 60 FPS as each scene allows. When 60 isn't possible it settles on a smooth 30 rather than a choppy 40.
- **Locked 30:** always 960×544, always 30 FPS when the Vita can manage it.

### Measured on a PS Vita (widest camera zoom, the toughest view)

| Galaxy | Quality | Performance | Locked 30 |
|---|---|---|---|
| Good Egg | 30 FPS, up to 960 wide | 54 FPS | 30, at 960×544 |
| Beach Bowl | 30 FPS, up to 784 wide | 30 FPS | 25, at 960×544 |
| Honeyhive | 30 FPS, up to 848 wide | 42 FPS | 30, at 960×544 |
| Bowser's Star Reactor | 29 FPS, up to 848 wide | 45 FPS | 30, at 960×544 |

At the default camera zoom, Performance mode hits 60 FPS in Good Egg and 54 in Honeyhive.

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
- Full details are in [Technical notes](https://github.com/CBOC-Vita-Lab/galaxy-vita/blob/main/docs/TECHNICAL.md) and [Source record](https://github.com/CBOC-Vita-Lab/galaxy-vita/blob/main/docs/SOURCE.md).

### Thanks
Several fixes and ideas come from Producdevity's fork of version 1.1: Star Bit reach, the right-stick pointer, the enemy-damage, hidden-star and rear-touch fixes, and some speed-ups. See [Credits](https://github.com/CBOC-Vita-Lab/galaxy-vita/blob/main/CREDITS.md).

**Updating?** Install the new VPK over the old one. Your saves and settings stay. Measured frame rates are in [Performance](https://github.com/CBOC-Vita-Lab/galaxy-vita/blob/main/docs/PERFORMANCE.md).