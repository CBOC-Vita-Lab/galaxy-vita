# Prepare Your Game

**This is the manual route.** [CBOC Vita Installer](DESKTOP-INSTALL.md) does
all of this for you. By hand, it works on Windows, macOS or Linux and takes
three steps: extract your disc, convert it, copy it.

The port reads the game's files in a form made for the Vita (decompressed,
converted, and sorted into small folders so the memory card finds them fast),
so a raw extraction isn't enough. Everything runs on your computer. Your disc
image is never changed. Use your own **Wii USA (`RMGE01`)** disc, the original
release (revision 0).

## 1. Extract Your Disc With Dolphin

1. Install [Dolphin](https://dolphin-emu.org/download/) and add the folder
   with your ISO, RVZ or WBFS to its game list.
2. Right-click the game and open **Properties**. Under **Info**, check
   **Game ID: RMGE01**.
3. Open **Filesystem**. Right-click **Data Partition** and choose
   **Extract Entire Partition...** into a new, empty folder (allow about 5 GB).

You get a folder with `sys/` and `files/` (sometimes inside a `DATA/` folder;
either is fine). Choose the entire **data partition**, not **Extract Files...**,
which leaves out the system files.

## 2. Convert It

You need [Python 3](https://www.python.org/downloads/) on your computer. Extract
the whole install ZIP, open Terminal or PowerShell in that folder, and run:

```sh
python3 tools/cook/cook.py "/path/to/extracted" prepared-game --vita-layout --with-movies
python3 tools/cook/vita_layout.py prepared-game --verify
```

On Windows, write `py -3` instead of `python3`. The first command takes a few
minutes. The second checks that every file is in its place. If either one
reports an error, extract the disc again into a new folder and repeat.

### Optional: Smaller Movies

The installer also re-encodes the nine movies at a lower bitrate: about half
the size (1.4 GB instead of 2.4 GB) and easier for the Vita to play. By hand,
on Mac or Linux with a C++ compiler and libjpeg:

```sh
g++ -O2 -std=c++17 -o thp_transcode tools/thp_transcode.cpp -ljpeg
for m in prepared-game/files/MovieData/*.thp; do ./thp_transcode "$m" "$m.new" 55 && mv "$m.new" "$m"; done
```

## 3. Copy It To The Vita

Copy `prepared-game/sys/` and `prepared-game/files/` into
**`ux0:data/smg/game/`** using [USB, FTP or a card reader](INSTALL.md#choose-a-transfer-method),
then go back to [Install](INSTALL.md). The final paths look like this:

```text
ux0:data/smg/game/sys/fst.bin
ux0:data/smg/game/files/ObjectData/_bk00/
ux0:data/smg/game/files/MovieData/
```

Don't add another `game/` or `DATA/` level, and keep the `_bkNN` folders as
they are. Keep your prepared files to yourself: they're made from your copy of
the game.

## Check Your Extraction

The SHA-256 of `sys/main.dol` in your extraction should be:

```text
2c680585a8f58e1cc9c5521b579057f12b124ff0ef409e470a57606c50a93c09
```

From the extracted folder, run `Get-FileHash -Algorithm SHA256 sys/main.dol` in
Windows PowerShell, `shasum -a 256 sys/main.dol` on macOS, or
`sha256sum sys/main.dol` on Linux. A different value means a different region or
revision of the game.
