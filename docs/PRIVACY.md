# Privacy and safety (multiplayer, version 3.0)

This page says in plain words what multiplayer shares, with whom, how safe it is, and how to share less. It is about the game's own
multiplayer and the online service that goes with it. It is not about your PlayStation account, which the game never touches.

It was written by reading the game's code and the service's code. Nothing here has been checked on a real PS Vita. If the code changes,
this page must be checked again.

**The short version.**

- **Nothing goes to the online service when the game starts or when you open the Multiplayer menu.** Builds made with the default
  address (`https://galaxy.psvita.dev`) first contact it only when you press an online button: **Play with Anyone**, **Invite from Far
  Away**, **Friends**, **Leaderboards**, or **Add Friend** / **Block** on a player's page. The first of those makes your account, if you
  have none: the game sends a display name and the answer to a small puzzle, and gets back a friend code and a secret token. Once you have
  an account, a finished Star Race time is posted to the leaderboards. Online play is not open to players yet, so the parts of this page
  about the service describe what *will* happen.
- **Wi-Fi, nearby and far-away-code play never use the online service.** In every kind of game, the other players see the name and
  character you chose. **Your name is your character's name** ("Mario", "Luigi", ...) until you give one of your own, and it follows the character you pick.
  The game does not use your PS Vita's own user name (it can be your real name); it never shows it to other players. There is no menu to type a
  name: to show your own, put `name = Something` and `name_typed = 1` in `ux0:data/smg/mp/mp.cfg` (a `name = ...` line from an older version that is
  not a character's name, "Player" or your console's user name is kept as yours; one that is your console's user name is replaced by the character's). Games played
  directly between consoles also need your console's network address (what a website sees when you visit it) to reach the other console.
- **What players send each other during a game is encrypted and checked.** Only the request to join, and the announcement of a game
  on Wi-Fi or Nearby, travel in the open. Read [How safe is it?](#how-safe-is-it) before you play with people you do not know.
- **Typed messages (up to 24 characters, on in every game) are not filtered.** Each player can hide another
  player's messages on their own console. **Player names are filtered** with one list shared by the game and the online service (rude or
  impersonating names are shown as the character's name); the list is not a guarantee.
- **There is no sign-up form, e-mail address or password.** The online service keeps a name, a friend code and a secret token for each
  account, and the times you post to the leaderboards. Everything it keeps is listed below.

## What leaves your console

| When | What is sent, and to whom |
|---|---|
| The gathering screen on Wi-Fi, nearby | To the other consoles in the game: what the game needs to play (see [What other players can see](#what-other-players-can-see)). To everyone on the Wi-Fi or in radio range: the announcement of a game you opened (name, mode, number of players). Nothing to any server. |
| **Invite from Far Away** (a code for a friend) | To the online service: the five-character code, and an encrypted copy of your console's network address and game secret. The service receives the code too, so it could open that copy. To your own router: a request to open one UDP port. To `stun.cloudflare.com` (Cloudflare): a request that asks what your public address is. Then, to the other consoles, as above. |
| Any online request to the service | Your console's address (every internet request shows it), the game version, a short fingerprint of the game build (8 letters and digits), the fixed name "GalaxyVita" as the program, and, once you have an account, your secret token. The sections below say what each kind of request adds. |
| Making an account | Your name (cleaned: letters, numbers, spaces, dots, dashes and underscores, at most 10 characters; "Player" if nothing usable is left) and the answer to a small puzzle. No region, e-mail address, console number, PlayStation ID or anything else. |
| Changing your name | The new name. |
| Friends | The friend codes you add, block, dismiss or remove, and a request for your lists. |
| Opening or joining an online game | Which game, and the addresses other consoles need to reach yours (see below). |
| After an online Star Race | Galaxy, star, your time, how many played, the game's code, the game version, and a short fingerprint of your run. |
| Reading leaderboards | The galaxy and star you are looking at, and which tab. |
| Who Can I Play With? (the Connection Test) | See [The Connection Test](#the-connection-test). |
| A problem report | Only if you choose **Having Trouble? > Send a Problem Report** and agree on its consent screen (the **What's Sent** page shows what goes): a cleaned copy of the game's log (at most 48,000 bytes) and a few numbers about the connection. Nothing is sent before you agree. See [Reports](#reports). |
| After an online game | When you leave an online game, a short **session summary** of numbers: how the connection ran (direct or relay), how long it lasted, how many players, packet-gap figures and the game version. No name, friend code or console address is in it, and it is stored without your account. This one is sent without a question. See [Reports](#reports). |

## What the online service keeps

You only have an account on the service if you use **Play with Anyone**, **Invite from Far Away**, **Friends**, **Leaderboards**, or **Add Friend** / **Block** on a player's page,
in a build that has the service address (the default). The game makes the account by itself, without a sign-up screen, the first time you press one of them
("Connecting..." while it works). It uses the name your game already has (see above: your character's name unless you set one in `mp.cfg`).

- **Your name.** Only a display name, cleaned to letters, numbers, spaces, dots, dashes and underscores, at most 10 characters ("Player" if nothing usable is left). Names do not have to be different from other players' names. The service refuses some words.
- **A proof of work.** When it makes your account, your console solves a small puzzle (a computer does it in a fraction of a second;
  nobody has timed it on a Vita) so that making thousands of fake accounts costs time. The puzzle is not made from anything about you
  and it is not kept.
- **A friend code.** Eight letters and numbers made by the service, shown as XXXX-XXXX. It lets friends find you. It grants nothing else.
- **A hashed token.** Your console gets a long secret ("token") when it makes the account. The service keeps only a one-way fingerprint (a hash)
  of it, so the service cannot read your token back. The token itself is kept on your console, in `ux0:data/smg/online/online.dat`.
- **A region number.** The service's table has one. The game does not send one, so every account gets the default (it is not taken from
  where you are).
- **When you were last seen,** and when you made the account. An account not seen for 365 days is removed. An account that was made
  and then never used (no call more than 5 minutes after sign-up, no friend, no time) is removed after 3 days, or sooner when the service is full. When the
  service is full, the account unseen longest (at least 60 days) also makes room for a new one. If your account is removed, your console makes a new one
  quietly, with a new friend code.
- **You can delete your account yourself, from the game.** The game's delete-account choice asks the service to remove your account at once:
  your name, friend code, secret token, region, last-seen time, your friend list and the requests and blocks you made (also from the other
  players' lists), your star times and any game or invite you hold. After that the service keeps nothing that names you. Backups the owner of the service makes of its data may still hold a deleted account until they are themselves deleted. What it keeps
  for abuse limits holds no name or code: daily counters keyed by a number that is never used again, the day's total of sign-ups, and the
  per-address request counters (which expire within a day or two). Your console forgets its saved account at the same time. You can make a new account afterwards; it has a new friend code, and your friends have to add it again.
- **Your Star Race times:** the galaxy, the star, the time, how many players raced, the game's code, the game version, and whether the
  time was checked ("verified"). The game also sends a short fingerprint of your run; the service stores it and does not compare it with
  anything, so it is not used to spot fake times. Only your best time for each star and number of players is kept as a record. Every
  submission is also kept for 2 days, to count how many you send and to check races. A time shows to other players only when it has been
  checked: a second player of the same game, on a different network, with an account at least 48 hours old, must have sent a matching
  time, and the time must fit in the time the game was running. A time you played alone, or that nobody confirmed, is seen only by you
  and by friends on the Friends board. A time that looks too good is kept, hidden from everyone but you, until an administrator looks at it.
  An administrator can also remove times or keep an account off the boards.
- **A public web page.** The service has a page, `/times`, that anyone can open without signing in. For one star it lists the checked times
  with the player's name, the number of players, and the date to the hour. It shows nothing else about the player.
- **Your friends and the players you blocked,** as lists of account numbers. A friendship counts only when both of you added each other.
  At most 50 friends and 200 blocked players. A request nobody answered is kept (at most 100 per player).
- **Whether you are online.** You count as online for one minute after your last request. Kept in the service's memory only, never in
  its database, and forgotten after an hour.
- **Daily counts per account,** overwritten each day: how many games you opened and how many relay passes you asked for. They are used to stop abuse.
  Relay passes you are given are also noted for a while, so that relay use can be reported back. If a time of yours looks too good, a note
  with your account number and which star is kept for an administrator to review.
- **Two counts in its database that are not about you:** how many accounts were made today, and how many requests the service has
  answered today. They are how the service keeps inside the free plan's daily allowance. The day ends at 00:00 UTC. At about 80 percent of the day's
  request count the service stops taking new games, sign-ups and friend calls, and keeps the rest for games already going; when the count runs out it
  says it is busy (the game shows "Online play is busy right now"), and your console then stops asking for up to 10 minutes.
  New sign-ups are limited to 10,000 a day in launch week and 2,000 afterwards (the limit halves and the puzzle gets harder as the service fills up).
  The daily limits on requests per address (and per account) are counted in the service's database, so that they still hold when the
  service restarts. What is stored is a number for the day, filed under a scrambled code made from your network address (for IPv6, from its
  first 48 bits) and the date, and under your account number for the per-account limit. The scrambling uses a secret key that stays on the service and is
  not part of any backup; your address itself is not stored in these counters and cannot be read back from them. They are kept for the day
  and deleted when the next day (UTC) begins; the scrambled code changes every day. One other counter, how many relay
  credentials your network address has asked for today, is stored in the database with the address and is deleted after 2 days.
  A game asks the service about 60 times per player-hour (the service tells the console how often to ask), so the day's allowance
  covers about 750 player-hours. Room codes that do not exist are counted against your network address, not your account: ten in 10
  minutes or 40 in a day and that address is told to wait.

### Your network address and the service

- **The service sees your address** every time your console talks to it, as any website would. It uses it to slow down abuse and to
  work out whether two players are behind the same router. The counters for slowing down (how often one address asked) are of two kinds: the short
  windows (minutes) live in the service's memory only; the daily ones are in the service's database as the scrambled, per-day counters described above (no address, only a daily code and a count; deleted when the day ends),
  with one exception: the count of relay credentials asked for from your address (see above) is stored with the address for 2 days.
- **While you are in an online game**, the service keeps the details other players' consoles need to reach yours: the public address
  and port your router uses, your console's local address, a relay address if one is used, and the address your last call came from.
  They are kept with your place in the game. It also keeps, for 15 seconds after you leave, which place you had, so you can take it back.
  (A member who goes silent is dropped from the game after a minute.)
- **They are removed** when you leave the game, or when the game is deleted. A game that ends stays on the service for 5 minutes
  (the default) first, so for up to about that long after a game closes, its players' details are still there.
- **The service does not write a log of requests.** The service's own code does not print or store request logs, and its
  Cloudflare settings leave request logging off. (Cloudflare, the company that runs the Worker the service lives on, handles web traffic like any web host,
  under its own rules. This page cannot speak for it.)

## What other players can see

**In every game:**

- **Your chosen name and your character** (and the look you picked). The player who opened the game sends the whole list to everyone.
- **A per-game id.** To tell your console apart inside one game (for example so a removed player stays out, so you can take your own
  place back after a Wi-Fi drop, or so another player's Hide Messages follows you if you come back), the game uses a number made for that game only. It is a scrambled mix of a secret random number
  kept on your console and the game's own number, so two games cannot be matched to one console. The secret itself is never sent.
  This id is in the request to join, which is not encrypted, so someone who can see the traffic at that moment can copy it. After the
  join it travels encrypted.
- **Your ping** (how slow your link is), and your moves, messages and what you do in the game. That is the game.
- **What you type** (up to 24 characters). Typed messages are on in every kind of game, including **Play with Anyone**, with no off switch for
  the leader. The leader's console reads each one before it goes on to the others. Quick phrases need no typing. The game does not filter
  words in typed messages (names are filtered; messages are not). Each player can hide another player's messages and balloons on their own console, by player (**START**, **Players**,
  the player, **Hide Messages**); that choice is kept in the console's memory for the session only.
- **Your progress.** When you join a game, you are sent the progress (stars, lives and so on) of the player who opened it. Anyone who
  joins gets this, so do not open a game to people you do not want to see your progress. Note that on the gathering screen every console on the same
  Wi-Fi that is on that screen can end up in your game.
- **Your friend code,** if you have an online account. The game tells the other players in your game, in every kind of game, so they can add you as a friend.
  It lets them send you a friend request and nothing else. The code you get from them is kept in your **Played With** list.

**Your network address, when you connect directly.** A direct connection means two consoles send packets straight to each other. A console
that receives packets can see where they come from. So:

- **Wi-Fi:** consoles on your own Wi-Fi network see each other's local address, for example 192.168.x.x. The player who opened the
  game also announces the game on that network (their name, the mode, how many players), so anyone on the same Wi-Fi network can see
  it, even if they have no Vita. Nearby (ad hoc) games are announced to any console in radio range in the same way.
- **Internet, through the service:** the consoles connect to each other directly if they can. The player who opened the game learns
  every guest's public address, and each guest learns that player's. A guest learns another guest's local address only if both are
  behind the same router. The service never gives a guest another guest's public address,
  with one exception: when the player who opened the game leaves or loses the connection and another player becomes the host, the
  new host receives the guests' public addresses (and the guests the new host's), as the first host did.
- **Internet, with a far-away code:** the code is what lets your friend reach your console. So **whoever you give the code to may learn
  your public address** (and the code is in a message you may have sent through a chat service), and the leader learns the friend's
  address as packets arrive. Give the code to one friend only. When a code goes through the online service, the service does not hand
  either side the other's address until the leader's console has accepted the joiner (your console accepts by itself today, so this
  protects against a stranger who guesses a code and only reads, not against one who joins).
- **The player list.** On Wi-Fi and nearby, the console of the player who opened the game sends everyone the local address of each
  player in the game, in the same list as the names (it is encrypted). In games through the service, and in games with a code, it sends no addresses.
- **The game does not show anybody's address on screen.** The code stands in for it. Messages
  in the game's log are another matter: see [Reports](#reports).
- **A relay.** If two consoles cannot reach each other directly, the traffic goes through a relay server instead, and then
  each console sees only the relay's address. The relay is a server the service names (Cloudflare's, in the default setup).
- **The service's game view also gives each member the account number** of the other members, next to their names (not their friend
  codes). It is not shown in the game.

**Your friends** can see whether you are online, in a game, or offline. Other players cannot see your friend list.

## What stays on your console

These are files on your memory card. The game does not send them anywhere.

- `ux0:data/smg/mp/mp.cfg`: your multiplayer settings and a secret random number (`console_key`). Do not share it.
- `ux0:data/smg/mp/flow.cfg`: your last menu choices and the names, and friend codes where the game learned them, of the last 20 players you played with ("Played With").
- `ux0:data/smg/mp/coop.cfg`: the names and characters of the players on each Team File.
- `ux0:data/smg/mp/saves`, `backups` and `recovery`: your Team Files, backups of single-player files, and recovery checkpoints. They hold game progress.
- `GameData.bin.mpbak` and, briefly, `GameData.bin.mptmp`, next to your single-player `GameData.bin`: the safe-save copy of your previous save. Game progress only.
- `ux0:data/smg/online/online.dat`: your online account (name, friend code and the secret token). Anyone who has this file can act as you on the service.
- `ux0:data/smg/mp-backups/`: safe copies of the files above, made when a new version of the game starts or when you choose **Make a Copy**. They hold your online sign-in too, so do not give
  them to anyone (a copy made with **Make a Copy to Share** leaves the sign-in out).
- `ux0:data/smg/log.txt`: the game's log, which can hold network addresses, codes and names. See [Reports](#reports).

## How safe is it?

This section is about the game as it is today, not as it is planned to be.

- **What players send each other is encrypted and checked, console by console,** in every kind of game: Wi-Fi, nearby, far-away code and
  online. A message that does not come from the player it claims is thrown away. Other people on the same Wi-Fi cannot read the chat, positions,
  progress or who is in the game, or change any of it unnoticed. Only computer tests have run this. Nobody has tried it on a console or on a
  real network, and the tests did not look for every kind of attack.
- **Not encrypted:** the request to join (name, character, per-game id) and the announcement of a game on Wi-Fi and nearby.
- **Someone on the same Wi-Fi, in radio range, or on the path of a game with a code can still** jam the game so it stops working for everyone, list
  fake games, see that a game exists, who opened it and how big the packets are, and, on Wi-Fi and nearby, answer a join before the real
  game does, so the player ends up in a fake game. They cannot get into your game this way.
- **The gathering screen trusts the Wi-Fi.** Every console on the same Wi-Fi that is on that screen for the same mode shows up as a face, and a game that
  has started is offered to every console on that Wi-Fi with the same version. You cannot remove a face before you press START; you can only leave.
- **There is no password.** Anyone who can reach a game can ask to join a free place; the far-away code is the only key there is, and it is only as private as you keep it.
- **A removed player can come back as a new console.** **Remove from Game** keeps a console out of this game, including after the leader
  changes, for as long as it keeps the identity it joined with (its saved key and session token); a console whose settings were wiped, or a
  different console, is a new player. It is for ending a disruption in a game among friends, not a permanent ban, and its list (eight
  consoles, the oldest forgotten first) ends with the game.
- **Typed messages are not filtered** (names are). Hide Messages hides one player on your console only.
- **Internet games through the service** use an encrypted link between consoles. Only the computer tests have run it, and online play
  is not open to players yet.
- **The leaderboard** cannot tell a real race from two accounts of one person who are patient, so a time on it is not proof of a fair race.

Only play with people you know, on a network you trust. If you do not, treat the list above as still true.

## Looking things up for you

When you press **Invite from Far Away**, the game talks to some things outside the game:

- **Your router.** It asks, with UPnP and with NAT-PMP / PCP, to forward one UDP port to your console, and names the request "Super Mario Galaxy".
  The request goes to your own router on your own network.
- **A public STUN server**, `stun.cloudflare.com`, run by Cloudflare. The request says nothing but your address and port; the answer
  tells the game its own public address. To never contact it, put `stun = 0` in `ux0:data/smg/mp/mp.cfg`.
- Internet games through the service use the STUN and relay servers the service names, when you open or join one.

## The Connection Test

**Who Can I Play With?** (the Connection Test) is offered after an online try fails ("Check who you can play with?"); a build made with `NP_ONLINE_URL=none` never offers it. It checks your Wi-Fi, the online service, hosting and the
delay. It is reachable without an online account. While it runs it contacts:

- **Your own router**, on your local network: the same UPnP and NAT-PMP / PCP request as a far-away code (port 41100, named "Super Mario Galaxy"),
  to see whether hosting would work. The test lets the mapping go when it ends, and never touches a mapping a game is using.
- **The online service**: one plain request for its status and version, sent without signing in. It carries no token, name or
  friend code; like any request, it shows the service your address. In a version with no service address, it makes no request.
- **A third-party STUN server**, `stun.cloudflare.com` (Cloudflare), to learn your public address, unless you put `stun = 0` in `mp.cfg`
  (or `SMG_MP_STUN=0` in `env.txt`).
- **Your Wi-Fi's other consoles**, with one broadcast on the local network, to look for a game (the Wi-Fi search).

If **Relay only** is on (see below), the test asks neither your router nor a STUN server, and does not test hosting: the screen says
"Relay only is on (address hidden)".

## How to stay local-only

- **Play on Wi-Fi or nearby.** The gathering screen on its own never uses the online service and never asks a STUN server or a router for anything.
- **Do not press Friends, Leaderboards or Play with Anyone,** and do not press **Add Friend** or **Block** on a player's page. Those ask the service.
- **Never make an account.** The game makes an online account by itself only when you press something that needs one. Without one, the game makes no
  call to the service. (Your name is your character's name, or the one you set in `mp.cfg`; it stays on your console and in the games you join.)
- **A far-away code without the lookups:** put `stun = 0` in `mp.cfg`, and `SMG_MP_UPNP=0` and `SMG_MP_NATPMP=0` in `ux0:data/smg/env.txt`.
  You then forward the port on your router yourself.
- **Internet play without direct connections ("Relay only"):** put `relay_only = 1` in `ux0:data/smg/mp/mp.cfg`, or `SMG_NET_RELAY_ONLY=1` in
  `ux0:data/smg/env.txt`. The service then gives out only the relay address, and your console shows other players the relay's, never your own.
  The Connection Test also stops asking your router and a STUN server. **Which one wins:** `relay_only = 1` in `mp.cfg` always forces relay only
  (`SMG_NET_RELAY_ONLY=0` cannot turn it off); `relay_only = 0` (the default) leaves `SMG_NET_RELAY_ONLY` in charge, where any value other than 0 means on.

## How to block, mute and remove

- **Block** a player who asks to be your friend: **Friends**, **Requests**, choose the request, then **Block** (**Square**). They can no longer send
  you requests, and no presence or game is shared with them. Blocking also ends an existing friendship. You can also block a player from inside a game
  (**START**, **Players**, the player, **Block**) or from **Played With** (**Square**), once you have an online account and the game has that player's friend code. To undo it, open
  **Blocked Players** and choose **Unblock**; that list shows the players you blocked since the game last started, so for an older block use **By code**.
- **Hide Messages** for a player in a game: **START**, **Players**, choose the player, **Hide Messages**. Their messages and balloons are hidden on your console, for that
  player (even if they come back in another place), until the session ends.
- **Remove from Game:** if you are the leader, open the player's page the same way and choose **Remove from Game**. Your console refuses
  that console for the rest of the game, even after the leader changes, as long as it keeps the identity it joined with. It is not a permanent
  ban: see [How safe is it?](#how-safe-is-it).
- Blocking does not stop someone from joining a game you are in. The leader controls who is in it, and each player
  controls what they see.

## Reports

**Problem reports (only when you send one).** In the Multiplayer menu, **Having Trouble? > Send a Problem Report** shows a consent screen first, and a
**What's Sent** page that lists what goes. If you agree, the game sends a cleaned copy of its log (at most 48,000 bytes) with a short note
about where it happened, the game version and a few connection numbers. Before it leaves the console, the game takes out addresses, friend
codes, room codes, names, tokens, your console's secret, and the names of servers and folders; the service cleans it again when it arrives.
The service keeps a report for 30 days and then deletes it; it is deleted soon after if you delete your online account. A masked copy also goes
to the service's own log, which has no name, token or address in it. Nothing is sent if you do not choose it.

**Session summaries (sent when an online game ends, without a question).** When you leave an online game, the game sends one small summary of
numbers with the leave request: whether the link was direct or through the relay, how long the game lasted, how many players were in it, packet-gap
figures and the game version. It has no name, friend code, account number or address in it, and the service stores it without your account.
At most one is kept per player and room, and the service keeps a small number of them in total.

**The log file on your card.** The game's log (`ux0:data/smg/log.txt`) can contain network addresses, codes and names. It stays on your card
unless you send a problem report (above) or attach it to an [issue](https://github.com/CBOC-Vita-Lab/galaxy-vita/issues). If you attach it
yourself, read it first and take those out. The game also has code that writes a cleaned file, `ux0:data/smg/report.txt`, from the log; the cleaning
code has been tested on its own tests and on eight game logs from play on a computer, and it has not seen the log of an online game.

## Running your own server

The service is a program in the `server/` folder of this repository. Anyone can run one on Cloudflare's free plan, and then the game
can use it instead: add `server = https://your.address.example` to `ux0:data/smg/mp/mp.cfg` on every console of the group. Your
group's data then lives on your server and nowhere else. The steps are in [server/DEPLOY-SMG.txt](../server/DEPLOY-SMG.txt), and
[MULTIPLAYER.md](MULTIPLAYER.md#running-your-own-online-service) tells players how to point their consoles at it. A token for one server is never
sent to another.
