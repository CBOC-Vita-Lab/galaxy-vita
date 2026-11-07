# Super Mario Galaxy Vita

This is Super Mario Galaxy running natively on a PS Vita. There's no emulator and no streaming involved: the game itself
was rebuilt to run on the Vita's own processor and graphics chip. We haven't found anyone who has run Galaxy on the Vita
before, so as far as we know this is a first.

Boot it up and you land in the Comet Observatory with Rosalina's music playing, holding the whole game in your hands. And it plays like it was made for the Vita: slide a finger on the rear touch pad to
aim Star Bits, touch the screen where you used to point the Wii Remote, and tilt the Vita to roll the Star Ball.

![Mario in Good Egg Galaxy on the PS Vita](store/screenshots/01-good-egg.png)

**[Download the latest version](https://github.com/CBOC-Vita-Lab/galaxy-vita/releases/latest)** · [Install guide](MANUAL-INSTALL.md) · [Controls](CONTROLS.md) · [What's new](CHANGELOG.md)

You'll need your own copy of the game (Wii, USA, `RMGE01`).

## New In Version 2.1

Far fewer pauses: the download now carries ready-made graphics from every mission of every galaxy, and each galaxy prepares what it needs while it loads.

## New In Version 2.0

The biggest update yet. Galaxy now runs at the Vita's full 960×544 resolution at a locked-in 30 FPS, and a new Performance mode pushes toward 60.

- **Full resolution, steady 30.** Quality mode draws at up to the Vita's native 960×544 and holds 30 FPS in every galaxy we tested, from Good Egg to the Bowser fights.
- **Up to 60 FPS.** Pick Performance mode and lighter galaxies run at 50 to 60 FPS. When a scene is too heavy for 60, it settles on a smooth 30 instead of a choppy 40.
- **Honeyhive, Beach Bowl and the Bowser fights hold 30.** These used to drop into the teens and 20s.
- **All four of the Vita's CPU cores.** With the CapUnlocker plug-in, the game spreads its work across every core the Vita has.
- **More control in the pause menu.** Camera zoom, rear-touch aiming, a right-stick Star Bit pointer and Star Bit reach, all one button away.
- **Gameplay fixes.** Enemies that grab Mario hurt him again like on the Wii, hidden-star missions work properly, rear touch works right after visiting Settings, and spins come out even right after a jump.
- **Plus everything from 1.2.** Bowser, the Lumas and about 85 effects look the way they should, a full Settings menu, no freezes on a galaxy's first visit, a 20-second start, music after sleep and full PS TV support.

The full list is in [What's New](CHANGELOG.md), and [Known Issues](KNOWN-ISSUES.md) covers what's still being worked on.

## Touch Controls, Built For The Vita

Galaxy was made for the Wii pointer, so touch got more work than anything else in this port.

- **Rear touch Star Bit aiming.** The feature people asked for most. Slide a finger on the back pad and the Star Bit
  pointer glides across the screen like a laptop trackpad, while your thumbs stay on the sticks and buttons. Fingers that
  are just holding the Vita are ignored. Turn it on in the pause menu with Triangle.
- **The front screen is your Wii pointer.** Touch anywhere and Star Bits fly to you. Tap to talk, open and choose, and hold
  a finger still to grab a Pull Star.
- **Tilt to steer.** The Star Ball and Manta-ray levels steer by tilting the Vita. Wherever you hold it counts as level, and
  Select re-centers it.
- **Buttons where the Wii used motion.** Spin with Square or R instead of shaking a remote, and shoot Star Bits with Circle.
- **Settings in the pause menu.** A panel in the game's own style: L and R set the camera zoom (1.0× to 2.0×), Triangle
  turns rear touch aiming on or off, Square makes the right stick move the camera or the Star Bit pointer, and Select sets
  how far from the pointer Star Bits are collected (Star Bit reach).

The full list is in [Controls](CONTROLS.md).

## How Smooth Is It?

Choose your style in Settings > Display > Picture mode:

- **Quality** (default): the sharpest picture at a steady 30 FPS.
- **Performance**: as close to 60 FPS as each scene allows.
- **Locked 30**: always full 960×544, always 30 FPS when the Vita can manage it.

Measured on a real PS Vita at the widest camera zoom, the toughest view in the game:

| Galaxy | Quality | Performance | Locked 30 |
|---|---|---|---|
| **Good Egg** | **30 FPS**, up to 960 wide | **54 FPS** | 30, at 960×544 |
| **Beach Bowl** | **30 FPS**, up to 784 wide | 30 FPS | 25, at 960×544 |
| **Honeyhive** | **30 FPS**, up to 848 wide | **42 FPS** | 30, at 960×544 |
| **Bowser's Star Reactor** | **29 FPS**, up to 848 wide | **45 FPS** | 30, at 960×544 |

At the default camera zoom, Performance mode hits 60 FPS in Good Egg and 54 in Honeyhive. For comparison, version 1.2 ran Honeyhive at 12 FPS from this view.

| | |
|---|---|
| **Start-up** | the title screen in about 20 seconds |
| **Download** | about 9 MB (2.5 GB on the memory card with your game files) |

Every galaxy, zoom and mode we measured is in [Performance](docs/PERFORMANCE.md).

![The Comet Observatory on the PS Vita](store/screenshots/02-comet-observatory.png)

## The Game

Bowser has taken Princess Peach and the Power Stars, and Mario heads into space to get them back. From Rosalina's Comet
Observatory you fly out to dozens of galaxies: planets small enough to run around in seconds, gravity that changes under your
feet, Star Bits everywhere, the Bee, Boo and Spring suits, and some huge boss fights. 120 Power Stars in all. Plenty of people
call it one of the best games ever made, and it plays wonderfully on a handheld.

## How It Works

- **It runs natively.** A Wii emulator would need far more power than the Vita has. Instead, the game's own code, from the
  community's [Petari](https://github.com/SMGCommunity/Petari) decompilation, is compiled for the Vita's ARM processor.
- **It has its own graphics engine.** The Wii's graphics commands are translated for the Vita's GPU as the game runs, and the
  work is shared across all three of the Vita's CPU cores.
- **It keeps full game speed.** The game runs its 60 steps a second. If a moment is too busy to draw every frame, a
  frame is skipped rather than the game slowing down.

## Install

**You need:** a PS Vita with homebrew (VitaShell), about 2.5 GB free, your own Super Mario Galaxy disc image (Wii, USA, `RMGE01`), and `libshacccg.suprx` set up on the Vita (most homebrew-ready Vitas already have it).

### Easy way: CBOC Vita Installer

Open [CBOC Vita Installer](https://github.com/CBOC-Vita-Lab/vita-installer/releases/latest), pick Super Mario Galaxy, choose your disc image, and press Install. It prepares the game files and sends everything to your Vita over Wi-Fi or USB. [Desktop guide](EASY-INSTALL.md)

### Manual way

1. Download `Galaxy-Vita.vpk` from the [latest release](https://github.com/CBOC-Vita-Lab/galaxy-vita/releases/latest).
2. Prepare your game files from your disc image, including shrinking the movies so they play smoothly ([Prepare your game](MANUAL-PREPARE-GAME.md)).
3. Copy them to `ux0:data/smg/game/`, and copy the VPK to your Vita.
4. Install the VPK in VitaShell and launch the bubble.

**Updating?** Just install the new VPK over the old one. Your saves and settings stay.

## Playing

| Button | Action |
|---|---|
| **Left stick** | Move |
| **Cross** | Jump |
| **Square** or **R** | Spin |
| **L** | Crouch / ground pound |
| **Circle** | Shoot a Star Bit at the pointer (the right stick aims it) |
| **Right stick** | Camera |
| **Triangle** | Camera behind Mario |
| **Touch screen** | Point, collect Star Bits, choose; hold still to grab a Pull Star |
| **Rear touch** (optional) | Slide to aim the pointer |
| **START** | Pause |

**In the pause menu:** L and R set the camera zoom, Triangle turns rear touch aiming on or off, Square switches the right stick between camera and pointer, and Select sets Star Bit reach. **Settings:** tap Settings on the game's LiveArea screen, before playing or while the game runs. Your choices are saved for next time. The full list is in [Controls](CONTROLS.md).

## Download Contents

| File | What it is |
|---|---|
| `Galaxy-Vita.vpk` | The game app for your Vita. Install it in VitaShell. |
| `Galaxy-Vita-install.zip` | The VPK plus the guides, the game-preparation tools, checksums and the full source. |

## For Developers

| | |
|---|---|
| [Building](docs/BUILDING.md) | Build the VPK from source |
| [Technical notes](docs/TECHNICAL.md) | The renderer, the runtime switches, and the engineering notes for each version |
| [Source record](docs/SOURCE.md) | What each release was built from |
| [Check the download](docs/CHECK-DOWNLOAD.md) | Verify the files against `SHA256SUMS` |

The game code (the Petari decompilation) is in `decomp/`, the Vita runtime in `vita/`, and the GX renderer in `platform/vgx/`.

## Credits

Built by AI agents (Claude), and directed and play-tested on real hardware by a human. It stands on the work of the [Petari](https://github.com/SMGCommunity/Petari) decompilation project, GalaxyQuest, vitaGL and the Vita homebrew community. See [Credits](CREDITS.md).

An unofficial fan project, not affiliated with Nintendo or Sony. You must own the game.
