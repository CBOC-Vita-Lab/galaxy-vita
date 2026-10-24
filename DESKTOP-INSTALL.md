# Easy Install With CBOC Vita Installer

**The game doesn't need this program.** It runs on the Vita by itself, and
[Manual Install](INSTALL.md) works from any computer. The installer just does
the preparing, copying and installing for you.

[CBOC Vita Installer](https://github.com/CBOC-Vita-Lab/vita-installer/releases/latest)
(version 1.1 or later) is one small download for Mac (Intel or Apple Silicon),
Windows and Linux. Nothing else to install: no Python, no Dolphin, no FTP client.

1. Open the installer. It opens in your browser.
2. On the Vita, open VitaShell and press **SELECT**. The installer finds the
   Vita on your Wi-Fi by itself. (USB mode, or the Vita's memory card in a card
   reader, works too and is faster.)
3. Click **Get** next to Super Mario Galaxy, or drop `Galaxy-Vita-install.zip`
   onto the window.
4. Choose your own game: a Wii USA (`RMGE01`) ISO, RVZ or WBFS, or a folder
   extracted with Dolphin. The installer checks it's the right disc.
5. Choose `libshacccg.suprx` if it asks. It doesn't ask if the Vita already
   has it.
6. Click **Install**.

Before anything is sent, the installer converts your game for the Vita on your
computer: every archive, model, sound and movie is made ready for the port,
every file is checked against the expected result, and the movies are made
about half the size so they play smoothly. This takes about 2–3 minutes on a
recent computer, once. Your own disc image is never changed.

Then it copies about 2.5 GB to `ux0:data/smg/game/` and installs the app. Over
Wi-Fi there's nothing to press on the Vita. Over USB or a card reader, finish by
pressing **Cross** on the VPK in VitaShell's `ux0:VPK` folder.

Your saves (`saves/`), settings (`settings.ini` and `env.txt`) and HD pack are
never touched. When a new version comes out, open the installer with the Vita
connected: it shows **Update** next to Super Mario Galaxy, sends only what
changed, and doesn't convert or send your game again. If a transfer stops,
press **Install** again; finished files are skipped.

Vita Wi-Fi manages about 2–4 MB/s, so USB is the quickest route for a first
install.

First launch help: [Mac](https://github.com/CBOC-Vita-Lab/vita-installer/blob/main/docs/mac-open.md).
On Windows, if SmartScreen appears, choose **More info > Run anyway**.
