# C186.1 emulator Settings smoke

This is a menu/relaunch test, not a physical Triangle, first-use, FPS or
graphics test. Use the **packaged** C186.1 VPK (SHA-256
`5b075db717b9e54397dd2bf5892a189ac9d8e09de258aee900b9bca23c41a554`).
The captured result is in `settings-emulator-receipt.json` beside the VPK.

1. Start with a **throwaway Vita3K profile**, private game/save fixture,
   and no pre-existing player settings or Vita title shader cache. Supply
   your own lawful firmware and cooked game data; none is in this kit. Do
   not use, overwrite, or link to a player's install, settings, or save.
   Use a separate app copy, filesystem, temp/config dirs, no PSN and no
   network. The tested macOS SDL/Vulkan build was Vita3K 4232, with a
   private app bundle ID/Metal cache and a pre-existing emulator-side
   pipeline cache copied from a disposable profile. On this host a missing
   host cache can cause a Vulkan varying mismatch before play; this is not
   a failed Settings assertion. Do not change the retail shader profile.
2. Install the VPK in the disposable profile only. In its
   `ux0:data/smg/env.txt` place the following exact lines. Do **not** set
   `GXV_PICTURE_MODE`: the boot must pick Performance by default and the
   native menu must have permission to change it.

   ```text
   PETARI_BOOT=1
   PETARI_STAGE=HoneyBeeKingdomGalaxy:1
   PETARI_STAGE_HOLD=20000
   PETARI_SCRIPT_ANCHOR=play
   PETARI_SCRIPT_LOG=1
   SMG_SETTINGS_PAUSE_TEST=1
   GXV_VSOUT=1
   GXV_STDERR_FILE=1
   ```

3. In that same disposable directory put the following single line in
   `input.txt`. Launch the game **once** with no controller/keyboard input.

   ```text
   3:PLUS,9:RIGHT,10:B,11:A,12-14:PX=0.42|PY=0.60,13.25:A,14.5-16:PX=0.68|PY=0.60,15.5:A
   ```

4. The script pauses; the diagnostic hook asks the native menu to open
   after 150 pause turns (bypassing raw Triangle). The script changes
   Picture, backs out, selects Save and Start Again, then points at Yes
   and confirms. Do not reopen the bubble or restart the process. After
   the new boot logs Quality, stop the private emulator before the input
   sequence can repeat. Keep both guest logs and the Vita3K runtime log.

   Evidence for this run: previous guest log has Performance, native
   `opened (pause)`, `restart asked (pause)`, `saved` and `restart now`;
   emulator log has `sceAppMgrLoadExec` on the original eboot with
   `-play` and `Relaunching in-process`; the fresh guest log has the same
   build and `Quality (30) (GXV_PICTURE_MODE=30)`. The previous log also
   contained a zero return from the relaunch call before the emulator
   processed it. Never use that line alone to decide success or failure.
   A host timer stopping the process after these observations is not
   evidence of a clean game shutdown.

To extract allowlisted observations from the two local guest logs:
`python3 tools/c186_console_receipt.py --platform emulator log.txt.prev log.txt`.
Use the physical run card separately for the visible hint and real Triangle.
