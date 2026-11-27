# Galaxy Vita C186 console A/B kit

Use `console-tests-C186.md` in this kit instead of the original run card.
Record the actual build and VPK SHA-256. Do not mix builds within a
comparison. The original C186 executable is missing native Settings hooks;
its pause-menu restart check cannot pass. C186.1 fixed the missing link;
C186.2 also reports presentation stalls. Neither an executable nor these tools
certify an on-device result.

Start with the backup/readback instructions on the card. The six complete
`env/<arm>/env.txt` files prevent an override from the previous run leaking
into the next one. The kit contains no VPK, save, shader cache, network
client or installer. Nothing here writes to the Vita.

**Optional local checks** (Python 3.8+, run in the unzipped folder):

```sh
python3 c186_console_arms.py check plant-full /path/to/copied/env.txt
python3 c186_console_arms.py compare /path/to/backup /path/to/second-read
python3 c186_console_receipt.py --platform console /path/to/log.txt.prev /path/to/log.txt
```

- `check` accepts exactly the selected assignments, with LF or CRLF line
  endings. A file match does not prove the game read it or that the menu
  mode, stage overrides and shader cache were correct.
- `compare` checks names, directory structure, file sizes and SHA-256 in
  two independently copied, closed snapshots. It reads but does not change
  them. A missing file, shared file (hard link), symlink, unreadable file
  or an entirely empty tree cannot give a false match. Use a separate
  inventory for paths that were absent before the session. Metadata such
  as modification times is intentionally not compared.
- `c186_console_receipt.py` prints a summary to standard output. Supply
  logs in chronological order and only from the named platform. Full logs
  stay private. The summary omits addresses, tokens, file names and raw
  messages. It does not verify the platform, on-screen behavior or cache.
  A first-use event log is **not a frame-time trace**. Don't add individual
  frame events to stage totals, or infer 60 fps from a short backend time.

**Reproduce from the QA source checkout:**

```sh
python3 -B -m unittest discover -s tests -p 'test_c186_console*.py'
python3 tools/c186_console_arms.py kit /new/path/C186-console-qa.zip
```

The ZIP command refuses to replace an existing file. For the full host
regression suite use `bash tools/test_c186.sh` with its pinned offline
dependencies. The visual A/B and genuine in-game timing still need a
physical Vita. Approval to install or to use an online room is separate.
