# Galaxy 3.0 practice partner

A small program for your Mac (or any Linux computer) that plays as a second player, so you can try multiplayer with only one Vita.
It runs in Terminal, finds the Vita on your home Wi-Fi, joins its room and then stays near your player: it copies your jumps a moment
later, says a phrase now and then, and moves its Star Pointer. It has no screen and no game files; it only speaks the same network
language as the game, so it must be built from the same source as the Vita's game (the program tells you if it was not).

**Status (20 Nov 2026).** Tried on a desktop against a stand-in for the Vita's Play Together gathering screen (`mp_bot --gather`: the same
"one door" code, `port/mp_gather.h`, that the screen drives): the partner found the gathering room, joined it, appeared as a second round face,
and the leader's Start began the session. Not yet tried against a real Vita or the real screen. Steps 2 and 3 say what should happen; if it
differs, note what you saw. It cannot use a far-away code, and it does not use the online service.

**Port (25 Nov).** The Vita's Same Wi-Fi room uses UDP port 24100 (the console cannot bind 41100), so the partner now uses 24100 too. `SMG_MP_UDP_PORT=<port> ./partner` overrides it (a desktop copy of the game still uses 41100). If the partner says `Looking for a room` for ever while the Vita's gathering screen is open, this was the cause in the 25 Nov console runs.

## 1. Build it (once)

Open Terminal, go to the unpacked `galaxy-partner` folder and run:

    ./build.sh

It uses the compiler that is already on the computer. If a Mac says there is no compiler, run `xcode-select --install` once (a
window opens; click Install, wait, then run `./build.sh` again). The build takes about a minute and ends with a line like
`partner: protocol 5, build id "dev", source ...`. The protocol number there must be the one in the game you
are playing (this tree speaks protocol 5: `MP_PROTO_VERSION` in `platform/include/port/mp_proto.h`).

If the bundle came with a `build-id.txt` and a `data-hash.txt` (make_source_bundle.sh writes them), nothing else is needed: the build uses the
Vita build's build id, and the partner knows the game-data hash. Otherwise: `MP_BUILD_ID=3.1-dev ./build.sh` and `./partner --data-hash aa3ddccb`.

(Working in the game's source folder instead? The same files live in `tools/mp/partner/`; `./build.sh` there makes `./partner` there.)

## 2. Start it

Put the Vita and the computer on the **same Wi-Fi** (not a guest network). Then, in the same folder:

    ./partner

The first time, macOS may ask "Do you want the application partner to accept incoming network connections?" Click **Allow**. If macOS
(version 15 and later) also asks to find devices on the local network, allow that too; if you said no by mistake, switch Terminal on in
System Settings > Privacy & Security > Local Network.

The first lines tell you the protocol version, build id, data hash and fingerprint it uses. The fingerprint must equal the one the Vita
logs (`[mp] core data hash=... fingerprint=...`; for the 3.1-dev test build with this disc: `aa3ddccb` and `1ec6b551`). Then it waits:
`Looking for a room on this network ...` (open Play Together on the Vita).

## 3. On the Vita

1. Start the game, open **Multiplayer** and choose **Play Together**. You land on the gathering screen with your own round face on it.
   Both of you must be on the same Wi-Fi.
2. Wait up to about 30 seconds. Terminal should say `Found room "<your name>"`, then `In the room as slot 1, character Luigi`. A second round
   face (the partner) should appear on the Vita's gathering screen. If nothing happens, use the fallback below.
3. Press **START** on the Vita. Answer the falls question if it asks, then pick a file on **Which File?**. Walk around in a galaxy: the
   partner appears a few steps behind you and does what you did a moment ago.

Other ways to connect:

* **The partner hosts** (checks of the gathering screen, character choice and joining only; it cannot pick a galaxy): `./partner --host`, then
  on the Vita open **Multiplayer**, **Play Together**. The partner's room should show as **Play with <name>**; press **Cross** on it.
* **By address**: `./partner --join 192.168.1.20` still exists (use the Vita's address from your router). A room opened with a spoken
  code needs a secret that the partner cannot make or read, so this does not work for code rooms.
* **Star Race**: `./partner --mode race` is the partner's setting when it hosts; it has not been tried against the new Star Race screens.

Stop it with **Ctrl-C** (it says goodbye to the room). It writes everything it does to `partner.log` next to the program.

## What you can try

    ./partner --name Alex2 --look toad     another name and character (mario luigi toad cosmic yellowtoad cosmicluigi)
    ./partner --wander                     walk a small circle near where you were instead of following
    ./partner --idle                       stand still
    ./partner --follow-delay 1500          follow further behind in time (milliseconds)
    ./partner --bubble-me                  falls into a bubble 20 s after you appear: touch it (or spin at it) to pop it
    ./partner --chat-every 10              a phrase and an emote every 10 s (0 = never); --pointer-every 8 for the Star Pointer
    ./partner --leave-after 60             leaves politely after a minute
    ./partner --drop-after 60              vanishes without saying goodbye after a minute (tests timeouts and host migration)
    ./partner --loss 20 --delay 80 --jitter 40   a bad network on what the partner sends (and 20% loss of the poses it hears)
    ./partner --help                       everything

Timings count from the moment the partner is in the room. The line at the bottom of the window shows the room, how many players,
your character, ping, how many poses per second it hears and what it is doing.

## If it does not work

* **"Version mismatch"**: the partner and the Vita's game were not built from the same source, or the game data differs. The message
  says what each side speaks. Build the partner again from the Vita's source. If only the data hash differs, pass the value from the
  Vita's log (the line `[mp] boot data hash=XXXXXXXX`): `./partner --data-hash XXXXXXXX`.
* **Nothing is found**: on the Vita, **Play Together** shows **Look for Consoles Nearby** after about 20 seconds; press it. Then: both on the same Wi-Fi? Some routers block devices from seeing each other ("AP isolation" / guest network);
  try `--host` and join from the Vita's gathering screen (or `--join ADDRESS`). Check `partner.log`: the line `broadcast targets:` should list your home network (like `192.168.1.255`);
  `send failed (errno 65 ...)` means macOS has not allowed Terminal on the local network (see step 2).
* **The partner joined but is not visible**: it only shows up once you are in a galaxy (the partner copies your position; there is
  nothing to copy in the menus). On the gathering screen you only see its round face.
* **Port in use**: another partner is already running; close it.

## What it leaves out

It never sends a Power Star (only a real star a player collects counts), it cannot choose or start a galaxy when it hosts, and it does
not use the online service, cannot read or make a far-away code, and does not open ports on the router. It does not press **Hide Messages**, ask for a
rematch or leave a Star Race results card; to try those you need a second console.
