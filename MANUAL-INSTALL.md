# Manual Install: Windows, Mac Or Linux

> **Most people should use [CBOC Vita Installer](EASY-INSTALL.md) instead.** It does every step below for you, with nothing to compile or install: pick Super Mario Galaxy, choose your disc, press Install. Use this guide only if you can't or don't want to use it.

**You don't need CBOC Vita Installer to install or play this game.** The VPK
runs on the Vita by itself, and any computer can prepare and copy the files.
Once it's installed, no computer is needed. Prefer one click?
See [Easy Install](EASY-INSTALL.md).

Use `Galaxy-Vita.vpk`, title ID `SMGV00001`.
[Start Here](START-HERE.txt) is the short version.

## You Need

- A homebrew-enabled PS Vita with VitaShell.
- Your own Super Mario Galaxy disc image: **Wii, USA (`RMGE01`)**, the original release (revision 0).
- `libshacccg.suprx` in `ur0:data/` (the shader compiler most vitaGL games need).
- About 2.5 GB free on the Vita (about 3.5 GB if you keep the movies full size; see
  [Prepare Your Game](MANUAL-PREPARE-GAME.md)).
- Python 3 on your computer, for the game preparation, and, only if you shrink
  the movies, a C++ compiler with libjpeg (the guide shows how to get them on
  Mac, Windows and Linux).
- **Recommended for best 60 FPS performance (optional):** **CapUnlocker** (unlocks the Vita's 4th CPU core) and **PSVshell / PSVshellPlus** (overclock to **500 MHz CPU / 222 MHz GPU / 222 MHz BUS**; the game automatically keeps your 500 MHz overclock profile).

## Installation

1. Download [the VPK](https://github.com/CBOC-Vita-Lab/galaxy-vita/releases/latest/download/Galaxy-Vita.vpk),
   or the [complete install ZIP](https://github.com/CBOC-Vita-Lab/galaxy-vita/releases/latest/download/Galaxy-Vita-install.zip)
   for the VPK plus guides, tools and source. **Don't unzip the VPK.**
   [Download checks](docs/CHECK-DOWNLOAD.md) are optional.
2. Prepare your game files on your computer with [Prepare Your Game](MANUAL-PREPARE-GAME.md):
   extract your disc and convert it. Shrinking the movies is optional, to save
   card space ([step 3](MANUAL-PREPARE-GAME.md#3-shrink-the-movies-optional-saves-about-1-gb)). You end up with a
   `prepared-game` folder holding `sys/` and `files/`.
3. Use one of the transfer methods below. Copy `sys/` and `files/` into
   `ux0:data/smg/game/`, and copy `Galaxy-Vita.vpk` into `ux0:VPK/`. Create
   missing folders.
4. Check for `ur0:data/libshacccg.suprx` in VitaShell. If it's missing, copy
   your own file there. **USB and card readers only show `ux0:`**, so put the
   file in `ux0:VPK/` first, then in VitaShell use **Triangle > Copy**, open
   `ur0:data/` and use **Triangle > Paste**.
5. In VitaShell, open `ux0:VPK/Galaxy-Vita.vpk`, press Cross and confirm.
   Launch the Super Mario Galaxy bubble. [Controls](CONTROLS.md) has the layout, including how surfing and
   the Star Ball steer by tilt, and what to do if your Vita's motion sensor doesn't work: the left stick
   can steer them ([Tilt](CONTROLS.md#tilt)).

The first launch takes a little longer while the graphics are prepared.

## Choose A Transfer Method

**USB:** In VitaShell, press START, set **SELECT button** to **USB**, and pick
the **USB device** that is your `ux0:` storage (for example SD2Vita). Close
the settings, connect a data USB cable and press SELECT. The drive that opens
on your computer is `ux0:`: use `data/smg/game/` and `VPK/` on it, not a new
folder called `ux0`. Eject the drive safely before you leave USB mode.

**Wi-Fi / FTP:** Put the Vita and the computer on the same network. In
VitaShell press START, set **SELECT button** to **FTP**, then press SELECT.
In an FTP client, connect to the address and port the Vita shows (FTP, not
SFTP), open `ux0:` and copy to the folders above. Keep VitaShell open until
everything has finished. USB is much faster for the 2.5 GB of game files.

**Memory card reader:** Only if your Vita's card (such as its SD2Vita microSD)
already works in your computer. Turn the Vita fully off before taking it out,
copy to `data/smg/game/` and `VPK/` at the card's root, eject it safely and put
it back. Never format the card for this.

## Folder Check

```text
ux0:data/smg/
  game/
    sys/
      fst.bin
      GalaxyID.bcsv       and the other prepared system files
    files/
      ObjectData/
        _bk00/ ... _bk57/ the bucket folders the preparation makes
      StageData/
      ...
  saves/                  your saves (made by the game)
  settings.ini            your pause-menu settings (made by the game)
  shadercache/            compiled graphics (made by the game)
  hdtex.pack              optional HD textures
```

`game/sys/fst.bin` must not sit in `game/game/` or `game/DATA/`. The `_bkNN`
folders are meant to be there: don't flatten or rename them.

## Updating

Install the new VPK over the old one. That's all: your game files, saves,
settings and compiled graphics stay where they are.

| Item | What to do |
| --- | --- |
| `Galaxy-Vita.vpk` | Install it in VitaShell over the old one |
| `game/` | Keep. Only copy it again for a first install or a repair |
| `saves/`, `settings.ini` | Keep. Back them up now and then |
| `shadercache/` | Keep. The game updates it by itself |
| `env.txt` | Optional switches. Only keep lines you added on purpose |

## If It Doesn't Start

- **Missing game files:** check that `ux0:data/smg/game/sys/fst.bin` exists and
  that you copied the *prepared* files, not the raw extraction.
- **Shader compiler error:** check `ur0:data/libshacccg.suprx`.
- **An error with `C2-12828-1` when the bubble starts:** this one error code has several causes, so
  check them in turn: `ur0:data/libshacccg.suprx` exists (it is set up once per Vita); the app installed
  is the one with title ID `SMGV00001`; and your disc image is the USA one (`RMGE01`), since other regions
  aren't supported. If it still happens, report it with `ux0:data/smg/log.txt`, the title ID and your
  disc's region.
- **The FTP or Linux file transfer fails or stalls:** use USB mode instead (it is also much faster), or the
  [installer](EASY-INSTALL.md). Problems with the installer itself belong on the
  [installer's issue page](https://github.com/CBOC-Vita-Lab/vita-installer/issues).
- **Surfing or the Star Ball won't steer:** your Vita's motion sensor may not work. See
  [Tilt](CONTROLS.md#tilt): the game switches those levels to the left stick by itself, or you can choose it in Settings.
- **Stuck on the first loading screen:** give the first launch a minute. If it
  never finishes, see [Known Issues](KNOWN-ISSUES.md).

When you report a problem, say which build you used and what you saw. Never
upload your disc image, game files or saves.
