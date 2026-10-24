# Game Preparation

What happens to your game files before they reach the Vita. It all runs on the
computer; the Vita never converts anything while you play.

## The Conversion

`tools/cook/cook.py` (and CBOC Vita Installer's `galaxy` step, which gives the
same files) turns an extracted `RMGE01` disc into the port's data:

- Every archive is decompressed. Models, layouts, collision, effects, sound
  tables and the data tables inside `sys/main.dol` are converted to the byte
  order the native game reads. Formats the game reads as byte streams (display
  lists, textures, sequences) keep their original bytes. Nothing is resized.
- The movies' headers are made little-endian; their pictures and sound are
  untouched.
- `--vita-layout` sorts the four biggest folders (ObjectData, StageData,
  LayoutData, AudioRes/Stream: 2,241 files) into `_bkNN` bucket folders of
  about 32 files, chosen by the FNV-1a hash of the lower-case name. The memory
  card reads a folder from the start on every lookup, so a lookup in ObjectData
  drops from 75-89 ms to a few. A flat copy also works: the game sorts it itself
  at the first start (about 5 minutes).

## Smaller Movies

The nine movies are about 2.4 GB as they come off the disc. The installer
re-encodes their pictures at JPEG quality 55 (1.37 GB, about half the size),
keeping the 640x368 size, every frame, the frame rate and the sound. Smaller
pictures also decode faster on the Vita. By hand, `tools/thp_transcode.cpp`
does the same ([Prepare Your Game](../PREPARE-GAME.md#optional-smaller-movies)).

## Checks

The supported disc is `RMGE01` (Wii, USA, revision 0); its `sys/main.dol` has
SHA-256 `2c680585a8f58e1cc9c5521b579057f12b124ff0ef409e470a57606c50a93c09`.
The installer checks every input file's size and every converted file's
SHA-256 against the expected result before anything is copied.
`tools/cook/vita_layout.py <folder> --verify` checks the bucket layout of a
manual conversion.
