#!/usr/bin/env python3
"""Packs the scripts a player needs into one zip for the release page.

  python tools/package_converter.py [output zip]   (default: out/GalaxyQuest-converter.zip)

To play, nobody needs the source: the APK and this zip are enough.  The zip
holds the disc converter (tools/cook/cook.py with the modules it imports)
and the script that copies the converted files to the headset
(tools/push_data.py), at the paths they have here, so the commands in the
README work as they are from the unzipped folder.  They only use Python's
standard library.

The file list is checked against what the scripts import, and the zip is
the same byte for byte whenever the files are (fixed dates, sorted names).
"""
import ast
import os
import sys
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDER = 'GalaxyQuest-converter'
ENTRY_POINTS = ['tools/cook/cook.py', 'tools/push_data.py']
# Where the entry points look for their modules (cook.py's sys.path).
MODULE_DIRS = ['tools/cook', 'tools']

README = """\
GalaxyQuest converter
=====================

These scripts turn your own extracted Super Mario Galaxy disc into the files
the GalaxyQuest app reads, and copy them to the headset.  They need Python
3.8 or newer and nothing else.

The commands below write "python".  On Windows type "py" instead (for
example: py tools/cook/cook.py extracted cooked --with-movies); on macOS
and Linux, "python3".  Run them in a terminal opened in this folder (on
Windows: click in File Explorer's address bar, type cmd, press Enter).

1. Extract your disc with Dolphin: right-click the game, Properties,
   Filesystem tab, right-click the disc at the top, Extract Entire Disc...,
   into an empty folder (here "extracted").

2. Convert the game files (a few minutes, 3.3 GB), from this folder:

       python tools/cook/cook.py extracted cooked --with-movies

   Without --with-movies the prologue and ending movies are left out (2.3 GB
   less) and play as a black screen.

3. Copy them to the headset, connected over USB, with adb (the Android
   platform tools) installed and GalaxyQuest.apk already on the headset:

       python tools/push_data.py cooked

   If it says "adb not found", tell the terminal where adb is first.  With
   the platform-tools unzipped into this folder, on Windows:
       set PATH=%PATH%;%CD%\\platform-tools
   and on macOS and Linux:
       export PATH="$PATH:$PWD/platform-tools"

   Without adb, copy the "cooked" folder to the headset with the computer's
   file manager (for example into Download) and pick it on the app's setup
   screen.

The full instructions: https://github.com/bigmak94/GalaxyQuest#installing
"""


def local_imports(path):
    """The repo files a script imports, by module name."""
    with open(os.path.join(REPO, path), encoding='utf-8') as f:
        tree = ast.parse(f.read(), path)
    names = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            names.update(alias.name.split('.')[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module and node.level == 0:
            names.add(node.module.split('.')[0])
    found = []
    for name in sorted(names):
        for directory in MODULE_DIRS:
            candidate = '%s/%s.py' % (directory, name)
            if os.path.isfile(os.path.join(REPO, candidate)):
                found.append(candidate)
                break
    return found


def needed_files():
    files, todo = [], list(ENTRY_POINTS)
    while todo:
        path = todo.pop()
        if path in files:
            continue
        files.append(path)
        todo.extend(local_imports(path))
    return sorted(files)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, 'out', 'GalaxyQuest-converter.zip')
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    files = needed_files()

    def add(z, name, data, executable=False):
        info = zipfile.ZipInfo('%s/%s' % (FOLDER, name), date_time=(2026, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = (0o755 if executable else 0o644) << 16
        z.writestr(info, data)

    with zipfile.ZipFile(out, 'w') as z:
        add(z, 'README.txt', README.replace('\n', '\r\n').encode('utf-8'))  # Notepad's line ends
        for path in files:
            with open(os.path.join(REPO, path), 'rb') as f:
                add(z, path, f.read().replace(b'\r\n', b'\n'), executable=path in ENTRY_POINTS)
    print('%s: %d bytes' % (out, os.path.getsize(out)))
    for path in files:
        print('  ' + path)


if __name__ == '__main__':
    main()
