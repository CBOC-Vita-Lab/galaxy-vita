# Check The Download

Extract the **whole install ZIP** into a new folder on your computer. Leave the
`.vpk` intact. The ZIP's `SHA256SUMS` lists every other file in it, including
the VPK, guides, tools and source.

## Optional Offline Checker

If you have Python 3, open Terminal or PowerShell in the extracted folder and run:

```powershell
# Windows
py -3 tools\check-files.py
```

```sh
# macOS or Linux
python3 tools/check-files.py
```

The checker reads local files only. It doesn't connect to the internet or the
Vita, install anything, repair files, or touch games or saves. `PASS` means
every file listed in `SHA256SUMS` matches. A missing or changed file means you
should extract a fresh copy into a new folder before installing.

## Without Python

Check the VPK with your computer's built-in command and compare the result with
its line in `SHA256SUMS` (letter case doesn't matter):

| Computer | Command, from the extracted folder |
| --- | --- |
| Windows PowerShell | `Get-FileHash -Algorithm SHA256 Galaxy-Vita.vpk` |
| macOS Terminal | `shasum -a 256 Galaxy-Vita.vpk` |
| Linux Terminal | `sha256sum Galaxy-Vita.vpk` |

A matching checksum shows the file arrived intact. It says nothing about game
compatibility or where the file came from, so get it from the project's
[release page](https://github.com/CBOC-Vita-Lab/galaxy-vita/releases).
