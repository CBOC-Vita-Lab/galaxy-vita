# Controls

Super Mario Galaxy was made for the Wii Remote and Nunchuk. On the Vita, every one of those moves has a home of its own.

## Playing

| Vita | What it does | (Wii) |
|---|---|---|
| **Left stick** | Move Mario | Nunchuk stick |
| **Cross** | Jump | A |
| **Square** or **R** | Spin | Shake |
| **L** | Crouch, ground pound, long jump, backflip | Z |
| **Circle** | Shoot a Star Bit at the pointer; hold and use the right stick to aim | B |
| **Right stick** | **Free Camera** (360° analog orbit & vertical pitch, default on Vita). Switchable to **Classic** or **Pointer** in **Settings > Camera** | D-pad |
| **D-pad** | Classic camera turns (left/right), first-person view (up), look back (down) | D-pad |
| **Triangle** | Tap once to center Free Camera behind Mario; tap again to return to the game's native camera | C |
| **START** | Pause | + |
| **Select** | Reset tilt (in tilt levels) | − |

### Free Camera (3.0.1)

On Vita handhelds, the right stick defaults to **Free Camera**: full analog 360° horizontal orbit and smooth vertical pitch with automatic wall and floor clearance.

- **Switch modes anytime** under **Settings > Camera > Right Stick**: **Free Camera** (default on Vita), **Classic** (original Wii D-pad camera turns), or **Pointer** (default on PS TV).
- **Camera Speed (`1–10`, default `5 Normal`)**: adjusts Free Camera turn and pitch speed.
- **Invert Camera**: **Off**, **Left / Right**, **Up / Down**, or **Both** in Free Camera mode (and **Off** / **Left / Right** in Classic mode).
- **Quick recenter & release**: press **Triangle** once to glide behind Mario; press **Triangle** a second time (within 1.5 s) to release back to the game's original automatic camera angle.
- **Star Bit aiming**: holding **Circle** temporarily hands the right stick to the Star Bit pointer and returns immediately to Free Camera on release.
- **Automatic yield**: Free Camera yields automatically during cutscenes, fixed/boss cameras, launch stars, first-person view (`D-pad Up`), and menus.

In multiplayer (see docs/MULTIPLAYER.md): **D-pad down** greets the others, holding the Star Bit button (**Circle**, or **R2** on a pad) or a finger on the screen shows them where you point,
and holding **Cross** while landing on a friend's head is a super jump.
Also in multiplayer, **START** opens the session menu, and **Select** held with a direction sends a quick
message (in the tilt levels Select still levels the tilt).

## Multiplayer controls

Only used while you are in a multiplayer session or its menus. Every button picture on the multiplayer screens is the Vita's own and follows
your button settings (**Settings > Controls**). Cross chooses and Circle goes back, as everywhere else in the game.

| What | Vita | Touch |
|---|---|---|
| **Move** in a menu | D-pad up / down, or the left stick | Touch a row, a big button or a head. |
| **Change a value**, page, galaxy or star | D-pad left / right, or the left stick | Tap the row. |
| **Choose**, Join, Start, Continue, Yes | **Cross** | Tap. |
| **Back**, Close, Leave, No, Cancel | **Circle** | Tap **Back** in the strip at the bottom. |
| **Third choice**: Add, Retry, Tab, Players, Change character, By address, Details | **Triangle** | Tap it in the strip at the bottom. |
| **Fourth choice**: Remove, Stamps, Kick, Test, Send report | **Square** (or **R**, or **SELECT**) | Tap it in the strip at the bottom. |
| **Session menu** in play (START again or Circle closes it) | **START** | |
| **Quick messages** | Hold **SELECT**, press a D-pad direction. One message every 2 seconds. In tilt levels SELECT still levels the tilt. | START, Stamps, tap a tile. |
| **Typed messages** (off until the player who opened the game turns them on) | START, Stamps, **Write a Message** (the Vita's keyboard) | Tap the tile. |
| **Say hello** | D-pad down (the right stick down too, when Right Stick is set to Classic) | |
| **Show your pointer** to the others, and hold an enemy for Co-Star | Hold **Circle** and aim with the right stick | Keep a finger on the screen. |
| **Watch** the previous / next player (when you are out and waiting) | **L** / **Square** (or **R**) | Tap a head of the strip. |
| **Pick a character** | D-pad or left stick, then **Cross** | Tap a head. |
| **Leaderboard tab** | **Triangle** | Tap it in the strip at the bottom. |
| **Super jump** on a friend's head | Hold **Cross** while landing | |
| **Pop your bubble** (near a teammate) | **Cross** | |
| **Back from "Be right back!"** (after 30 seconds with no buttons, or the quick message) | Any button | Touch the screen. |

With a pad on a PS TV, the pointer is the right stick, the Star Bit button is **R2** (so you hold **R2** to show your pointer), **R** is **R1** and **L** is **L1**.
The same table, with what the game code reads for each, is in `decomp/include/Game/MP/MpControls.hpp`; a host test (`tests/mp/vitabtn_build.sh`)
checks that no multiplayer text or picture names a Wii control and that every row has an input.

## PlayStation controller (PS TV)

With a PS3 or PS4 controller on a PS TV, the controls are laid out for a pad:

| Controller | What it does |
|---|---|
| **Left stick** | Move Mario |
| **Right stick** | Move the pointer |
| **R2** | Shoot a Star Bit at the pointer |
| **D-pad** | Camera: left and right turn, up for first person, down to look back |
| **Cross** | Jump |
| **Square** or **R1** | Spin |
| **L1** | Crouch, ground pound, long jump, backflip |
| **Triangle** | Camera behind Mario |
| **START** / **Select** | Pause / reset tilt |

This is the default when no profile has been chosen by hand in **Settings**. On PS TV, **Right Stick** defaults to **Pointer** and can be switched to **Free Camera** or **Classic** anytime under **Settings > Camera > Right Stick**. Every button can be changed under **Settings > Controls > Change Buttons**, which also lists L2, R2, L3 and R3 for a pad. The Vita's own controls are as in the table above.

**Mouse-style aiming (Settings > Controls, off by default).** The stick still sets how fast the pointer moves, but small pushes move it slowly and exactly, a full push sweeps across the screen, and the pointer speeds up the longer you keep pushing. It works the same at 30 and 60 FPS.

## Touch

- **Front screen:** your pointer. Touch to point, and Star Bits fly to you. A quick tap presses A where you touch (open, choose, talk). Hold a finger still to grab a Pull Star.
- **Rear touch aiming (optional):** slide a finger on the rear pad to move the pointer by how far you slide, like a laptop trackpad. It never jumps to where your finger is. Aim with one finger in the middle of the pad: a short, deliberate slide takes the pointer, and it lets go when the finger rests. Fingers holding the console (the outer quarter of the pad on each side) and two fingers moving at once are ignored. It's off until you turn it on, and it pauses while a finger is on the front screen.

## Tilt

Star Ball (rolling ball) and Manta-ray (surfing) levels steer by tilting the Vita. Wherever you hold it when the level starts counts as level. Press **Select** to make your current grip the new level.

**Steering with the left stick instead.** In **Settings > Controls**, the row **Tilt with** chooses between **Motion sensor** (the default) and **Left stick**; **Tilt sensitivity** (1 to 10, 5 is the game's own feel) applies to both. Settings opens from the pause menu (**△ Settings**) or from the **Settings** choice on the Single Player / Multiplayer screen, and the tilt choices apply at once. With the stick, the lean is the stick's push (the stick no longer moves Mario in those levels).

**If your Vita's motion sensor is dead.** Some consoles report a motion sensor that gives no readings, so tilting does nothing. Since 2.3 the game notices this in the tilt levels: if the sensor's readings stay completely frozen while you move the left stick for about 4 seconds, or the sensor doesn't answer a fully pushed left stick held for 1.5 seconds, the left stick steers instead (in the second case until the level ends, or until the sensor answers after all). Your saved setting is never changed, and `log.txt` gets a line starting with `tilt:` when it happens. A console that is lying still but works is not mistaken for a dead one, because a working sensor's readings normally jitter a little. To turn this off, add `GXV_TILT_AUTOSTICK=0` to `ux0:data/smg/env.txt`. You can also choose **Left stick** in Settings yourself and skip the wait. A console with no motion sensor at all (a PS TV) always uses the left stick.

## Menus

The left stick and the D-pad move the pointer on the game's menus, and Cross chooses. Touch works too.

## Pause Menu Settings

Open the pause menu with **START**. It has two lines:

| Button | Setting |
|---|---|
| **L / R** | Camera zoom, from 1.0× to 2.0× (default 1.5×) |
| **Triangle** | **Settings**: opens the game's Settings menu |

The rear touch, right stick and Star Bit reach settings that used to be pause-menu hotkeys are now rows in Settings (Camera, Controls and More).

## Settings

There is no Settings tile on the LiveArea any more. Open Settings in the game: choose **Settings** on the Single Player / Multiplayer screen, or press **△ Settings** in the pause menu. An updated console may still show the old tile until the game is reinstalled; tapping it opens the in-game Settings. In a multiplayer session the pause menu opens a small personal set (camera, controls, Less Motion, Star Bit reach); Picture, Language and Change Buttons wait until you are not playing together.

Settings are saved when you leave with **Save**.

**Removed rows.** These no longer appear and set themselves: Game speed (always full), Sharpness (automatic), HD textures (on when the HD pack is installed), Depth of field (on), Core plan (every part, when the console allows), Renderer (direct), Logic overlap (on), Material updates (the standard way), Far animation (on), **Safe saves (always on)**, and Layout / Profile (chosen from the controller in use). Testers can still force the old values; see [Tester switches](docs/TESTER-SWITCHES.md).
