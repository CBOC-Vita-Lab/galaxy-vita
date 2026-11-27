# Multiplayer (version 3.0)

Play Super Mario Galaxy with up to three friends. Everyone plays on their own PS Vita, in the same galaxy or in the same race.

> **Status, in plain words.** Multiplayer is new in 3.0. It has run on one real PS Vita: Play Together on Wi-Fi, with the second
> player joining from a computer (menus, gathering screen, play, checkpoints and leaving all worked). Two Vitas together, Nearby,
> far-away codes and four players have been tried only on computers, with several copies of the game running at once. The list of things nobody has seen on a console yet is in [KNOWN-ISSUES.md](../KNOWN-ISSUES.md).
>
> **The online service.** Builds made with the default settings connect to **`https://galaxy.psvita.dev`**, which is **live for Version 3.0.0** (**Play with Anyone**, **Friends**, **Star Race Leaderboards** at `https://galaxy.psvita.dev/times`, and Cloudflare TURN relay). A build made with `NP_ONLINE_URL=none` has no online features, unless the console's own settings name a server (see [Running your own online service](#running-your-own-online-service)). Games with friends on the **same Wi-Fi**, **nearby** (no router), or **far away with a code** do not need it.
>
> **Read [Playing with strangers](#playing-with-strangers) before you let anyone you do not know into a game.**

Some on-screen words still say "room" or "host" in a few older screens. This guide says "game" and "the player who opened the game"
(the **leader**, marked with a crown on the gathering screen) wherever it can.

## Quick start: play with a friend in 60 seconds

Everyone needs the same version of the game. There is no Host or Join to choose: you all go to the same screen and the game puts
you together.

### 1. You are on the same Wi-Fi

1. **Everyone:** after the title screen, choose **Multiplayer**, then **Play Together**. The first time, the game asks you to choose your
   character.
2. A screen shows a round face for each player it finds. Your friends' faces pop in by themselves. The first player (marked with a
   crown) is the leader.
3. **Leader:** when the line says **Press START to start!** (it shows when at least two players are there), press **START**.
4. The first time, the game asks "If someone falls, how should they come back?" (**In a Bubble** or **Try Again**) and then
   **Which File?** (see [Saves](#saves)). In the Observatory the leader picks the galaxy and everyone follows.

Want a race instead? Choose **Star Race** in place of **Play Together**. Everything else is the same, except that a Star Race never
asks **Which File?**

### 2. Your friend is far away

You need an internet connection and a router that lets the game in. See [Far-away friends](#far-away-friends-the-code).

1. **Leader:** on the gathering screen, choose **Invite from Far Away**. After a few seconds ("Getting your code...") a short code
   appears on the screen. Read it to your friend or send it in a message.
2. **Friend:** **Multiplayer**, then **Enter a Friend's Code**. Type the code (capitals or small letters).
3. **Leader:** press **START** when your friend's face has appeared.

The code works like a key: only someone who has it can come in. Give it only to the friend you mean it for.

### 3. Two Vitas side by side, no Wi-Fi at all

This is **nearby** play. It has never been tried on real Vitas (see [KNOWN-ISSUES.md](../KNOWN-ISSUES.md)).

1. Put the two consoles close together and turn Wi-Fi off on both (a Vita cannot do Wi-Fi and nearby play at the same time).
2. **Everyone:** **Multiplayer**, **Play Together**. With Wi-Fi off, the game looks for consoles nearby by itself.
3. If Wi-Fi is on and nobody is found for about 20 seconds ("No one was found nearby."), the button **Look for Consoles Nearby**
   appears. Choose it. **Look for Friends on Wi-Fi** switches back.
4. **Leader:** press **START**.

### 4. A game is already going

If your friends have already started and you are on the same Wi-Fi, a button **Play with <name>** appears on the gathering screen
while you are alone on it. Choose it to join their game.

## What you need

- A PS Vita running this version of the game. Every player must have the **same version**. A console with a different
  version is not put together with the others. (If a friend's code leads to a different version, the game says it could not reach your friend's game.)
- Your own Super Mario Galaxy file. Multiplayer keeps its progress separately from your single-player file (see
  [Saves](#saves)).
- For Wi-Fi play: all consoles on the same Wi-Fi network. For nearby play: the consoles close to each other (no router).
  For a far-away friend: a connection to the internet and a router that allows it.

## The two modes

**Play Together (2 to 4 players).** Everyone is in the same galaxy and the same scenario, and sees the others as Mario,
Luigi or another character. The leader picks the galaxy in the Observatory as usual, and everyone is taken
along. Switches, purple coins and Star Bits are shared, and so are the defeats of ordinary enemies. When one player gets a
Power Star, it counts for everyone, and every console plays the star-get celebration together. A player who falls floats back to a
friend in a bubble, or tries again (the falls question, below). Bumping into each other is always on, and a game always has room for four.

**Boss fights are not shared.** Each Vita runs its **own** copy of the boss. Your hits never hurt the boss on a friend's screen, and
theirs never hurt yours. The first player to win the fight gets the star, and the star ends the fight for everyone: players who are
still in the middle of a fight are taken to the star celebration. Think of a boss as a race to the star. (Sharing the damage is
possible future work; it is not in 3.0.)

**Star Race (2 to 4 players).** Everyone plays the same scenario on their own copy of the world, starting together on the
Cosmic Mario countdown (3, 2, 1, GO). The others appear as ghost racers. Your race timer shows on screen. When you get the Power
Star, your time is recorded and you wait, with the standings, until everyone has finished or until **3 minutes** have passed
since the first player finished.

Then the **results card** appears on the star screen. It lists the places and a **Series** column of points, which add up from
race to race for as long as the same people stay in the same game (first place gets the most; a player who did not finish gets
none). The player who won the last race starts the next countdown a few seconds late, so the others can catch up: 3 seconds after
one win in a row, 6 after two, 9 after three. The leader chooses what comes next with one press:
**Race Again**, **Next Star** or **Leave Race**. Every other player has one row they can press, **Leave Race**, which takes only
them out of the race. The card shows no countdown. One thing moves on by itself: if nobody touches the leader's card for 90 seconds,
the game starts **Race Again**, so a leader who walked away cannot hold the others (anyone can still leave). It does this twice at
most: if nothing was pressed or asked in between, the third time it chooses **Leave Race** (back to the galaxy map) instead of racing
for nobody. Any press on the leader's card, or a rematch ask from a guest, starts that count again.

**Guests can ask for a rematch.** A guest's card has two rows, **Race Again?** and **Leave Race**. Choosing **Race Again?** tells
the leader ("<name> wants to race again") and the card shows **Asked! Waiting for <name>...**. A guest can ask once per card, and
the leader still decides. (The ask is the quick phrase "Let's go!" while a results card is up, so sending that phrase by hand is
the same ask.)

## How to open the menu

After the title screen, the game shows two big buttons: **Single Player** and **Multiplayer**. The game remembers your last choice.
Choose **Multiplayer** to open the Multiplayer menu. It has these buttons:

- **Play Together** and **Star Race**: go to the gathering screen (below).
- **Enter a Friend's Code**: type the code a far-away friend read out.
- **Continue <name>'s Game**: only for about two minutes after a game dropped you (see [If you drop out](#if-you-drop-out)).

**How to Play** (**Triangle**) opens a short page with the controls of a game. **Friends** (**Square**) is there only in a build with the
online service address (the default). **Circle** goes back. The round two-player button on the file select screen is gone.
(If you add `SMG_MP_MENU=0` to `ux0:data/smg/env.txt`, the multiplayer menus are switched off and that button comes back.)

If a game ended suddenly last time, the Multiplayer menu first asks about the recovery checkpoint (see [Saves](#saves)).

### The gathering screen

This is the one screen where games are made. It shows a face and a name for each player found, a crown on the leader and **You**
under your own face once someone else is there. One line at the bottom says what is happening ("Waiting for friends...",
"Press START to start!", "Waiting for <name> to start..."). At most three big buttons appear, and only the ones that can do something now:

| Button | When it appears |
|---|---|
| **Play with <name>** | You are alone, and a game that has already started is open on your Wi-Fi. |
| **Invite from Far Away** | You are the leader. Shows a code for a friend far away. |
| **Play with Anyone** | You are alone and the online service is there. Joins any open game, or opens one for others to find. |
| **Leaderboards** | Star Race only, you are alone and the online service is there. |
| **Look for Consoles Nearby** / **Look for Friends on Wi-Fi** | Nobody was found for about 20 seconds, and the console can look the other way. |

Underneath: **START** starts (leader, two or more players). **Square** is **Change File** (leader, Play Together, once you have
played on a Team File before). **Triangle** is **Character**. **Circle** leaves; if others are there it asks "Leave the game?"

**Who ends up together.** On the same Wi-Fi, every console that is on this screen for the same mode at the same time is put together,
and the leader is chosen by the game. **Look at the faces and names before you press START.** There is no way to take someone off
the gathering screen before START: if a face is not wanted, leave with **Circle** and start again, or remove the player once the game
is going (see [Messages](#messages)). A game that has already started is also visible, as **Play with <name>**, to every console on the
same Wi-Fi with the same version.

### During a game

During a game, press **START**. It opens the session menu and the game keeps running behind it. The session menu has
**Go to a Friend** (Play Together, when others are there), **Return to the Observatory** (leader, Play Together) and
**Leave Multiplayer**. **Messages** is on **Square**, **Players** on **Triangle**.

Nothing about a game can be changed from the session menu. The things the old Room Settings screen had are now fixed: always four
places, bumping on, ordinary enemies shared, other racers as ghosts, a 3-minute wait in a race. The falls question is asked once (see
[Bubbles and the falls rule](#bubbles-and-the-falls-rule)).

## Controls (Vita)

Every multiplayer screen shows the Vita's buttons, and the touch screen works on every list, face and button. Cross chooses and Circle goes
back, as in the rest of the game. **Everything in this table has been seen only in screenshots and tests on a computer, never on a console.**

| What | Vita | Touch |
|---|---|---|
| Move in a menu | D-pad up / down, or the left stick | Touch a row, a big button or a face |
| Change a value, page, galaxy or star | D-pad left / right, or the left stick | Tap the row |
| Choose, Start, Continue, Yes | **Cross** (**START** starts a game from the gathering screen) | Tap |
| Back, Close, Leave, No, Cancel | **Circle** | Tap **Back** in the strip at the bottom |
| Add, Retry, Tab, Players, Character, How to Play | **Triangle** | Tap it in the strip |
| Messages, Remove, Block, Change File, Friends | **Square** | Tap it in the strip |
| Session menu in play | **START** | |
| Quick message | Hold **SELECT**, press a D-pad direction | START, Messages, tap a phrase |
| Typed message | START, Messages, Write a Message (the Vita's keyboard) | Tap it |
| Say hello | D-pad down | |
| Show your pointer, hold an enemy (Co-Star) | Hold **Circle**, aim with the right stick | Keep a finger on the front screen |
| Watch another player | **L** (previous), **Square** or **R** (next) | Tap a face in the row |
| Super jump | Hold **Cross** while landing on a friend's head | |

Lists show three rows at a time. Touch: tap a row once to point at it, tap again to choose it; on the Messages page the first tap
points and the second sends. With a pad on a PS TV the Star Bit button is **R2** and the pointer is the right stick. The same table is in [CONTROLS.md](../CONTROLS.md#multiplayer-controls).

## Playing together

- **Say hello:** press **down on the D-pad** while standing still. Your character strikes a pose and calls out, and the others
  see "Over here!". One greeting a second at most.
- **Show a spot, and help (Co-Star):** hold the Star Pointer button (**Circle**; **R2** on a PS TV pad) and aim, or keep a finger on the front screen. The others
  see your pointer, in your colour, where you point. Point it at an enemy and it freezes, on every screen, as with the second player's
  pointer in the original game (one enemy per pointer, up to 4 seconds, then a short rest of 2.5 seconds; bosses, switches and
  people are never affected). Star Bits close to a friend's pointer fly to you too. Let go and the pointer disappears.
- **Talk:** see [Messages](#messages).
- **One Star Bit count:** in Play Together every Star Bit anyone picks up counts for the team: one number on everybody's Star Bit
  counter, for Hungry Lumas and for the 1-Up at every 50 (everyone gets the 1-Up when the falls rule is Bubble; with Try again
  there is none). The leader's file gets the team's count; guests' own saves are never touched. Coins and health stay personal.
- **Super jump:** land on a friend's head while holding **Cross** for a big spinning spring bounce. Without Cross it is a
  normal stomp. Your friend gets squashed for a moment either way, never hurt. Bumping is always on.
- **Go to a friend:** in the session menu choose **Go to a Friend**.
- **Return to the Observatory:** the leader can take everyone back from the session menu.
- **Leave:** **Leave Multiplayer** in the session menu. A guest is asked to confirm. If the leader leaves, see
  [When the player who opened the game leaves](#when-the-player-who-opened-the-game-leaves).
- **A star while you are in your own star celebration.** If a second, different star is won while your console is already playing a
  star celebration, it is still counted for you: it is recorded and the other player's name is announced, but your console does not
  play a second celebration. A star that the game itself does not record (two odd spots in the game have one) is not shared at all, and
  nobody waits for it.
- **Waiting for a friend who is still loading.** If everyone is in a bubble, the group restarts together; a friend whose galaxy is still
  loading counts as not caught yet, for up to 40 seconds, so you are not sent back before they have arrived. A star that is
  handed to you while you are riding a Launch Star or a Pull Star waits (up to 12 seconds) until you land.

- **One world, kept in step by the leader.** Switches, enemies you defeat, Purple Coins and Star Chips are shared in Play Together (the
  enemy, Purple Coin and Star Chip sharing follow the game's rules for the session). A friend who joins later, or whose galaxy was
  rebuilt, gets the leader's current state of the galaxy a moment after arriving: defeated enemies stay defeated, collected coins and chips
  stay collected. If a friend's console disagrees with the leader's about an enemy (for example an enemy that is defeated on the leader's
  console but came back on theirs, as a few enemies do by design), the leader's state wins within about two or three seconds: the enemy
  is defeated again on the friend's console. When the leader is the one whose enemy came back, the friends' consoles are not changed:
  the leader's console is the reference, so the difference lasts until that enemy is defeated again. The "[mp] world drops" line in the
  log says how many events a console could not apply in each galaxy and why.

When you play on your own single-player file (**Play This File**), other players' stars count for your file more carefully. See [Saves](#saves).

The other players' faces appear in a row next to your life counter. A head goes dim, with signal bars, when a player has a weak
connection (or is away). It goes dark when they are down, and pale blue when they are in a bubble. When someone gets a Power Star in
Play Together, the faces of the players who have it stand under the game's own star banner (there is no separate Team Star card).

### Small touches

- **Where are we going?** When the leader picks a galaxy, everyone sees a card such as "Luigi picked Honeyhive Galaxy, Star 2."
  While they are still choosing, you see "Mario is choosing a galaxy." or "Mario is choosing a star."
- **The rules card.** When you join, a short card shows who opened the game and the rules ("If you fall, a bubble takes you to a
  friend.", or for a race "Whoever gets the Power Star first wins!"). Press **Cross** to close it. If the rules change later, you see
  a short notice. Bumping is not on the card because it is always on.
- **One colour for each character.** Mario is red, Luigi green, Toad blue, Yellow Toad yellow, Cosmic Mario purple and Cosmic Luigi
  teal. Name tags, pointers, speech balloons and lists use the same colour. Name tags are always on.
- **Away.** Choose the quick message **Be right back!** or stand still for 30 seconds: your face dims. Move or press a button to
  come back. It never stops anyone else.
- **Sounds.** A chime when someone joins and a sound when someone leaves.

### Watching a friend

When you are waiting and cannot play, the camera can follow a teammate instead of leaving you staring at a bubble or a results
screen. The view starts by itself after a moment if you are in a bubble with a long way to float, in a bubble with no lives left, or
in a Star Race after you finish while the others are still running. The screen says "Watching <name>" and shows the **L** and **Square** pictures once. Press **L** (previous) or **Square** or **R** (next) to switch. Touching a friend's face in the face row picks that friend.

### Bubbles and the falls rule

The first time the leader presses **START** in Play Together, the game asks "If someone falls, how should they come back?" The answer is
remembered and the question is not asked again:

- **In a Bubble**: a fall costs a life, and the player floats in a bubble to the nearest teammate. The stick
  nudges the bubble. A teammate pops it by touching it, or the player presses **Cross** when the bubble is near a teammate
  standing on the ground. A bubble can also be popped by a spin or a Star Bit hit. If everyone is caught in bubbles, the
  group restarts from the checkpoint together. A player with no lives left cannot pop their own bubble: it stays until a teammate
  pops it (and then they have 1 life) or until the next area.
- **Try Again**: a fallen player restarts from the checkpoint, and loses no life.

There is no screen to change the answer afterwards. To be asked again, delete the line `falls_asked = 1` from
`ux0:data/smg/mp/flow.cfg` (see [Settings files](#settings-files)). Star Race is not affected: a fall
there restarts you from the checkpoint as in the single-player game.

## Far-away friends: the code

A friend far away uses a short code instead of an address. The game never shows an address on screen. The code is what lets a friend
reach your console, so treat it like a key: give it to one friend. Anyone who has it can ask to join, and whoever you give it to may
learn your public internet address.

- **Leader:** **Invite from Far Away**. The game finds your public address (it asks a public STUN server, `stun.cloudflare.com`, run
  by Cloudflare) and asks your router to forward one UDP port to your Vita (port 41100, or the next free one up to 41107). A "port" is
  a numbered door on your router. It tries two ways at once, **UPnP** and **NAT-PMP / PCP**, and the router sees the request as
  "Super Mario Galaxy". The code appears in a few seconds, big on the gathering screen, with the line "Tell your friend this code."
- **Friend:** **Multiplayer**, **Enter a Friend's Code**. The text stays in the box if the code did not work, so you can fix it.
  The game checks Wi-Fi first. "That code doesn't look right." means a typing mistake. "We couldn't reach your friend's game." means
  the code was fine but the leader's router (or provider) did not let the game in. Check the code once more, or let your friend be the leader.
- Some routers, mobile hotspots and shared networks block this. A provider that shares one public address between many homes cannot be reached from outside.
  Then the friend who is not behind such a network should be the leader.

This has been tried only against pretend routers in tests on a computer. It has not been tried on a real router. You can turn the
automatic parts off with the `stun` key and the test switches in [Settings files](#settings-files).

## Characters

Each player plays a different character. There are six: **Mario**, **Luigi**, **Toad**, **Yellow Toad**, **Cosmic Mario** and
**Cosmic Luigi**.

The first time, choose yours on the **Choose Your Character** screen. If someone already has it, you will see **Already taken** and
cannot choose that face. On the gathering screen press **Triangle** (**Character**) to change it.

## Messages

- **Quick messages:** press **START**, then **Square** (**Messages**), and choose one of twelve friendly phrases ("Over here!",
  "Thanks!", "Wait for me!", "Let's go!", "Hello!", "Help!", "Yes!", "Which star?", "Sorry!", "No!", "Let's do this one!",
  "Be right back!"). Each player sees them in their own language.
- **In play:** hold **SELECT** and press a direction (up, down, left or right) to send one of your four most used phrases
  without stopping. Let go of SELECT to close it without sending. (In the levels steered by tilting the Vita, SELECT
  levels the tilt instead.)
- What you send appears in a speech balloon above your character for a few seconds, and as a line at the top of the
  screen with your character's head.
- **Write a Message** (under the phrases) opens the keyboard for a short message of your own, up to **24 characters**: one line of
  the balloon. The rules:
  - **Typed messages are on in every kind of game,** including **Play with Anyone**. There is no switch for the leader to turn them off.
    Each player decides what *they* see with **Hide Messages**, below.
  - The text has to be plain: no control or hidden characters, no spaces at the edges or doubled, and no more than two accent marks
    stacked on a letter.
  - The leader's console sees every typed message before the others do. Do not type anything private.
  - **The game does not filter typed words** (player names are filtered, messages are not). A typed message from another player can say anything that fits in 24 plain characters.
- One message every 2 seconds (a few in a row are fine).
- **Hide Messages:** press **START**, **Players** (**Triangle**), choose a player, then **Hide Messages** to hide their written messages
  and balloons on your console. Choose it again (**Show Messages**) to bring them back. It only affects your console. It is kept for
  that *player*, not for their place in the game, for the rest of the session: if they drop out and come back, even into another place,
  they stay hidden, and a new player who takes the empty place is not. It is forgotten when the session ends.
- **Remove from Game:** the leader opens a player's page the same way (in play) and chooses **Remove from Game**. That
  console is kept out of this game, including after the leader changes, for as long as the console keeps the identity it joined with
  (the saved key and the session token). A console whose settings were wiped, or a different console, is a new player and can come
  back. It is a way to end a disruption in a game among friends, not a permanent ban: when a ninth player is removed the game
  forgets the oldest of the eight, and the list ends with the game. See [Playing with strangers](#playing-with-strangers).

## Friends and blocking

Friends need the online service, which is not open to players yet. When it is open in your version, **Friends** is a button
on the Multiplayer menu (**Square**).

- Your friend code is shown on the **Friends** screen. Share it so friends can add you. The game also tells the other players in your
  game your friend code, in every kind of game, so they can add you.
- To add a friend, press **Add** (**Triangle**) and type their code. You become friends once they add you too. A friend request that
  has been made to you appears under **Requests**. For each one you can add them, choose **Not Now** (they can ask again), or **Block**.
- Friends see when you are **Online**, **In a room** or **Offline**. A friend's page shows **Play with This Friend** only while that friend is in
  a **Play with Anyone** game. Games made from the gathering screen (Wi-Fi, nearby, far-away codes) are not visible to the service, so friends find each
  other with the code instead.
- **Add Friend** and **Block** are on a player's page in a game (**START**, **Players**, the player), in every kind of game, as long as
  you have an online account and the game has received that player's friend code.
- **Block:** a blocked player's requests are dropped and no presence or game is shared with them. Blocking also ends an
  existing friendship. **Blocked Players** lists the players you blocked **since the game last started**, and **Unblock** is there. (Earlier
  blocks still hold on the service, but are not listed; **By code** on that screen unblocks by typing a code.) Blocking does not stop someone from joining a game
  you are in: for that, the leader can remove them, and you can hide their messages.
- **Played With** lists the last 20 players you have played with, in any kind of game, with **Add** (**Cross**) and **Block** (**Square**) on each
  row. It is kept on your console, with the friend code where the game has learned one.
- The **Friends** and **Friend Requests** lists ask the service again by themselves while they are on screen: at most once every 30 seconds, a slower pace after an error (60, 120, then 240 seconds; four errors in a row stop it; no "Couldn't load" over a list you already have), and not at all after 3 minutes without a button press or after 20 requests in one visit. **START** (**Refresh**) asks at once and starts the timing over. This is one request each time (2 a minute at the fastest, at most 6 if you leave it alone, at most 20 in a visit), kept small because the online service has a daily allowance.
- The service allows 50 friends. The Friends screen shows them three rows at a time.

## Leaderboards

After a Star Race, your time for each star goes to the leaderboard, if the online service is available in your version. **Leaderboards**
is a button on the gathering screen of **Star Race**, shown when you are alone on it.

- Times are sent **only from Star Race, and only from an online game** (**Play with Anyone**). A race played
  over Wi-Fi, nearby or with a code does not post a time. The service refuses a time that is impossibly short (under 10 seconds) or
  over 2 hours. A time that looks too good is kept but hidden from other players until it has been checked.
- The screen has three tabs: **Top** (the fastest times of all players), **Near Me** (times near yours) and **Friends** (times of
  your friends). Choose a galaxy and a star to see its times. A star with no times shows **No times yet.**
- A time in a race with friends shows on the boards for other players only after a second player of that game, on another network,
  with an account at least two days old, sent a matching time for the same star. A time you played alone is seen only by you and
  your friends. These checks make cheating harder, but the service still cannot tell a real race from two accounts of one person, so
  a time on the board is not proof of a fair race.
- Checked times, with the player's name, also show on a public web page of the service, which anyone can open without signing in. See [PRIVACY.md](PRIVACY.md).

### When online play is busy

The online service runs on a free plan with a daily allowance of requests. It spends that allowance carefully, and when it runs short the game says so instead of failing silently:

- New games and new accounts stop at about 80 percent of the day's allowance. Games already going keep their own share, so a game
  that has started is not cut off by a crowd that arrived later. If you join the game late, after it started, you are counted as new.
- When it is full you see **"Online play is busy right now. Please try again later."** or, when a daily limit of your own was
  reached, **"Online play is very busy today. Please try again tomorrow. Friends in the same room can still play."** The game then
  stops asking for up to 10 minutes. The day starts over at 00:00 UTC.
- The service decides how often a game asks it anything, so a game costs it very little: about **60 requests per player-hour** (a
  settled four-player game measured 26 in a test with the real service code). That makes room for about **750 player-hours a day** within
  the share of the allowance this game uses (45,000 requests a day); a player-hour is one player in a game for an hour. Before this
  change it was about 354 requests per player-hour, or about 170 player-hours a day. These are figures from tests on a computer, not
  from a live service.
- **Wrong codes cost the network address, not the account:** after ten room codes that do not exist in 10 minutes, or 40 in a day, from
  one network address, the service answers **"Too many tries in a short time."** to that address.
- **An invite does not give out your address at once.** When someone joins your game by an invite code, the service shows neither side
  the other's address until your console has accepted that joiner. Your console does this by itself (there is no question to answer
  yet), so what it protects against today is a stranger who guesses a code and walks away, or only reads. Friends and public games are
  accepted at once.
- **Names are checked** by the service and by the game with the same list (see [Playing with strangers](#playing-with-strangers)).
- Wi-Fi, nearby and code games do not use the service at all, so they work whatever the service says.
- A new account can be refused for the day when the day's sign-up allowance is used up (10,000 a day in launch week, 2,000 afterwards);
  accounts that already exist are never affected.

## Saves

Multiplayer is built so that it does not change your single-player file unless you choose to. **Back up `ux0:data/smg/saves`
before your first multiplayer game** (copy the folder to your computer). Ways in which multiplayer could change that file by
mistake were found by outside reviews and have been fixed in the code and tested on a computer; nobody has tried them on a console.

- **Team Files.** Progress made together is kept in Team Files, in `ux0:data/smg/mp/saves`, separate from your single-player file
  (`ux0:data/smg/saves/...`). The first time a leader presses **START** in Play Together (and when no Team File has been played yet),
  the game asks **Which File?**:
  - **Team File** (1, 2 and so on) continues one that already exists. One **New Team File** is always offered under them.
  - **Start from My File** asks which of your files 1 to 6, then **Play on a Copy** makes a Team File that
    starts from your single-player progress. Your file is not changed.
  - **Play This File** saves the game into your single-player file. A backup is made first, and you are asked to confirm.
    A friend's Power Star counts for your file only by the rules under **Playing on a single-player file**, below.
    **This has only been tested by unit tests and computer runs, never in a game on a console.**
  After that, the Team File played last goes on by itself, and **Change File** (**Square** on the gathering screen) chooses another.
- **Safe Copies.** The ordinary way in is under **How to Play**: on the Multiplayer menu press **Triangle** for **How to Play**, press **OK**,
  and the second page asks "Keep My Multiplayer Data Safe ... Open it now?"; **Yes** opens **Safe Copies**. (That is three presses from the
  Multiplayer menu. **Triangle** on **Which File?** still opens it too.) **Make a Copy** saves your Team Files and your Friend Code (the
  online sign-in) in `ux0:data/smg/mp-backups/`, **Make a Copy to Share** does the same without the Friend Code, and **Bring Back a Copy**
  lists the copies (yours and the ones the game made at an update) and puts back a Team File, your Friend Code or your settings, one at a
  time. **Bring Back a Copy** is also offered right where a file cannot be read: as a row on **Which File?** when a Team File is
  unreadable and a copy exists, and in the list that follows a crash. It is left out while there is no copy. The game asks first and saves
  what is there now. The copy holds your online sign-in: do not give it to anyone (the one "to Share" is safe to hand over).
  If the game is still writing a save when you ask, it waits for it for up to 15 seconds; after that you are told "Please wait a moment
  and try again. Nothing was changed."
  **None of this has been tried on a console.**
- **Whose file is it?** The leader's progress is the progress of everyone. When a player joins, that progress is sent to
  that player. After each Power Star, it is sent again. Guests never save during a game. So everyone sees the same galaxies and stars.
  In computer test runs, a guest's single-player file and a Team File leader's single-player file were never changed.
- **Playing on a single-player file (Play This File).** Progress goes into your own file only from things your own console saw. Another
  player's Power Star counts for your file only once that star has appeared in **your** copy of the galaxy (the game waits up to 3
  seconds for it to turn up). A star somebody else's console just claims, with nothing on yours to match, is refused. Two kinds of star
  can never appear in your copy by themselves, because every console runs its own boss and its own hidden spots: a hidden star or a boss
  star, and a grand star. For those the game accepts the claim only when the star belongs to the scenario you are playing, the other
  player's last position says they are in the very same scene as you, and they have not sent too many claims. A claim the game finally
  refuses (no such star in this scenario, it did not turn up within 3 seconds, or four other claims are already waiting) is never silent:
  both players see **"<name>'s Power Star couldn't be saved this time. Sorry!"**. What a cheating player could still do with this: credit
  your file with a star of the scenario you are in. They cannot credit a star of another galaxy or scenario, or more than the limit allows.
  The leader decides each claim once, on its own clock, and tells every console; the other consoles count a star only on that answer.
  A star a friend takes in the 5 seconds after the leader has already moved on (for example, while their screen was still loading the move)
  is judged against the scene the leader just left, as it was when the leader left it, and only if the friend was in that scene; a grand
  star taken that late is refused with the same message. The full rule is in `docs/STAR-CLAIM-RULE.md`.
  A snapshot of the leader's progress that is exactly the same as the one you already applied is not applied twice.
- **Leaving early.** When you leave a game, you are asked **Save this progress to a Team File?** Choose Yes, then
  a Team File to keep it in. **No** keeps nothing.
- **Recovery.** While you play, the game writes a recovery checkpoint (after each Power Star and from time to time). After a crash
  or a lost connection, it is meant to ask **Your last game ended suddenly. Keep its progress?** The checkpoints are written in
  computer test runs. **The question itself has been tried only in unit tests and has never been seen in a game.** The rules the game
  follows: a checkpoint of a single-player file that is blank is never offered, and one that has fewer Power Stars than the file now
  has, when the file was saved after it, is set aside (in `ux0:data/smg/mp/recovery/discarded`, ten kept) instead of being offered. If
  you do choose to put a checkpoint into a single-player file that has more Power Stars or was saved more recently, you are asked
  **"Your file N has more Power Stars, or was saved more recently. Replace it anyway?"**, and without your **Yes** nothing is written.
- **Backups of your single-player file.** Before multiplayer writes into a single-player file, it keeps a backup in
  `ux0:data/smg/mp/backups/`. The ten newest are kept. Put one back by hand.
- If a Team File or your file has more Power Stars than the progress you are about to save, you are asked before it is replaced.
- **Safe copies at an update.** The first time you start a new version of the game, it makes one checked copy of your Team Files, settings and online sign-in in
  `ux0:data/smg/mp-backups/`, before multiplayer writes anything. If the copy cannot be made, nothing is changed and it is tried again at the next start.
  A copy is a new folder; it is never written over. These show up in **Bring Back a Copy**.
- **How a save is written.** While a multiplayer session is active, every save is written to a temporary file first, checked, and then renamed, so a cut does not leave half a
  file. For the **ordinary save of your single-player file during play** this is **off by default** (the game's own in-place write); turn it on with **Safe saves** in Settings (Display page) or `SMG_ATOMIC_SAVES=1` in `env.txt`. In computer tests,
  power was cut at many points and no file was left damaged (without it, 22 of 300 random cut sequences left the single-player file damaged).
  - **Play This File:** the session writes your single-player file through the safe writer even when **Safe saves** is off. When the session is over and Safe saves is off, the `GameData.bin.mpbak` it made is removed again (one that was already there is kept).
  - **What you will see:** next to `GameData.bin` in `ux0:data/smg/saves/title/00010000/524d4750/data/` there is now a second copy of
    your previous save, `GameData.bin.mpbak` (about 48 KiB). A file ending `.mptmp` exists only for a moment while saving, or after a power cut
    in the middle of a save; the game finishes or undoes it at the next start. A file ending `.mpbad` is a damaged file kept aside for you.
  - **What it costs:** a save takes about 4.5 to 6 milliseconds instead of 1 to 2 on a computer (it reads the file back to check it), and the
    extra `.mpbak` copy uses 48 KiB. How long it takes on a Vita has not been measured.
  - **Switch it on or off:** Settings, Display page, **Safe saves** (applies from the next launch); or put `SMG_ATOMIC_SAVES=1` (on) or `SMG_ATOMIC_SAVES=0` (off) on a line of `ux0:data/smg/env.txt`, which wins over the Settings choice. It is read when the game first saves.
  - **Caveats:** the game puts a missing `GameData.bin` back at the next start (or, after a failed restore, the next time it reads the file) only when a `GameData.bin.mptmp` is beside it (a save that was cut in the middle). A
    `.mpbak` alone is never restored: if you delete `GameData.bin` by hand, the game starts without it and says so in `log.txt`; delete the old `.mpbak` too if you want the space back. If you go back to an older version, it
    ignores the sidecar files and saves in place as before.
    A file named `GameData.bin.mpbak.old` (or `.old.1`, `.old.2`, ... when there are several) is a previous copy of your save; renaming it to `GameData.bin` brings it back.
  - **If the card stops answering:** the recovery checkpoints are written by a background writer so that the game does not wait for the
    card. Whenever the game has to wait for that writer it waits at most 15 seconds. If the card has not answered by then, the game stops
    waiting instead of freezing and treats the file as not usable right now; no new checkpoint is started until that write ends.
  Nobody has tested any of this on a console.

## When the player who opened the game leaves

If the leader leaves, the game goes on. The player in the lowest remaining place becomes the new leader (the
screen says **"<name> left. <name> chooses the galaxy now."**). Nobody has to start again. Only the player who first opened the game can
save during a game. When you leave later, you are asked to keep the progress in a Team File, as described in [Saves](#saves).

This was seen with three copies of the game on one computer: when the leader's program was stopped, play went on after about
5 seconds. It has not been tried with lost packets, or on consoles.

The game ends only when everyone else has left, with **Everyone else left the game.** The list of removed players (up to eight) is
sent to every player, so it survives a change of leader: a console the old leader removed stays out under the new one. The new leader
does not know their names, only what identifies them.

### If you drop out

**Waiting for the leader.** After a Power Star, a player whose celebration is over waits behind the closed screen until the leader chooses where to go next (up to a minute). After about 2 seconds a small line, **Waiting for <name>...**, says who it is; it needs no button and goes when the game moves on. A player in a bubble sees the same line, naming a friend whose galaxy is still loading, until the bubble pops or the group restarts. Other waits have their own words: the start of a galaxy shows the **Waiting for players...** card, a race shows the standings, and a drop shows **Reconnecting...**.

If your Wi-Fi drops, the console goes to sleep, or the link just goes quiet, the game does not give up at once. It shows **Reconnecting...**
and tries to put you back in the same game, in the same place, with the secret only your console knows. After 10 seconds the line says
**Still trying to reconnect...**. How long it tries:

- when the Vita itself says Wi-Fi was lost or it woke from sleep: about 30 seconds;
- when the link simply goes quiet: up to about 75 seconds, with short pauses between tries. The leader removes a player it has not heard
  from for 15 seconds, and then keeps that place free for 70 more seconds (about 85 seconds after your last word), so the two sides cover
  the same time. A newcomer who finds the game otherwise full is told that the game is full; the place is held for the returning player
  alone, who always gets their own place back (the game cannot tell the newcomer why).

If it works, play goes on where it was. If it does not, you see **"The connection was lost. You can go back in from the Multiplayer menu for
two minutes."**, and the Multiplayer menu has **Continue <name>'s Game** at the top for two minutes. If the leader said goodbye or the game is gone, you are told the game ended instead, and there is
nothing to continue. If your game data did not match the others, the game says "Your game data didn't match the other console's, so you left the game. Nothing on your console was changed."

All of this has been shown on computers (with the link cut by the test harness), never on a Vita and never on a real Wi-Fi network. Longer
freezes, especially in a busy game with three players, can still drop the player.

## Playing with strangers

Be careful about who joins your game. This is how the game works today.

**What is protected.** In every kind of game (Wi-Fi, nearby, far away with a code and online), what the consoles send each other once
someone has joined is encrypted and checked, console by console. A message that does not come from the player it claims is thrown away.
Someone else on the same Wi-Fi, in radio range or on the internet cannot read your game, make a fake Power Star, remove a player,
freeze a player's character or take over a place. Two things are sent in the open: the request to join (it holds the name and
character a player chose, and a number made for that game only) and, on Wi-Fi or nearby, the announcement of a game (its name, the mode and how many players). This has been tried only on computers, never on a console or on a real network.

**What is not protected.**

- **Someone on the same Wi-Fi or in radio range can still jam the game** (it stops working for everyone), list fake games, or get in the way while someone is joining.
- **The gathering screen trusts the Wi-Fi.** Every console on the same Wi-Fi that is on the gathering screen for the same mode appears as a face, and
  a game that has started is offered as **Play with <name>** to every console on that Wi-Fi with the same version. There is no password and no
  "allow" question. Look at the names before you press **START**; you cannot take a face off the gathering screen, only leave. In a game, the
  leader can use **Remove from Game**. On a Wi-Fi or nearby game, a stranger could also answer a join before the real game does,
  so a friend ends up in a fake game instead of yours. They cannot get into your game this way.
- **Anyone who has a far-away code can join,** and anyone can join a **Play with Anyone** game. When someone joins, they receive the progress of the leader.
- **A removed player can come back, as a new console.** **Remove from Game** keeps that console out of this game, including after the
  leader changes, for as long as it keeps the identity it joined with (the saved key and the session token). Someone who clears their
  settings file, or uses a different console, is a new player and can join again. It is a way to end a disruption in a game among
  friends, not a permanent ban: a ninth removal forgets the oldest of eight, and the list ends with the game. The game's secret for a game
  is not changed after a removal.
- **Your friend code is shown to the people in your game,** so they can add you as a friend. It lets them send you a friend request
  and nothing else. They see it from your console; the game cannot tell if it is really yours, so do not trust it as proof of who someone is.
- **Typed messages are not filtered** (see [Messages](#messages)). Quick phrases need no typing. **Names are filtered:** the game and the
  online service share one list of rude and impersonating names, and a name on it is shown as the character's name instead. The filter is a
  list, not a guarantee.
  To hide a player's messages and balloons: **START**, **Players**, choose the player, **Hide Messages**. It only affects your console.

The safest way to play is with people you know, on a network you trust.

## Who Can I Play With? (the Connection Test)

If an online try fails (**Play with Anyone**, or **Play with This Friend** from a friend's page: no Wi-Fi, the online service cannot be reached,
a timeout, a router that blocks the game), the game offers to check your connection. It asks **"Check who you can play with?"** Answer **Yes** and a screen called **Who Can I Play With?** checks
three questions one after another: **People in this room**, **A friend far away** and **Anyone online**. Each ends with **Good**, **May be choppy** or
**Not right now**, and the line at the bottom says in plain words what to do ("Move closer to your Wi-Fi.", "Friends can't reach you here. Let a friend start the game.",
"Please try again later."). **Cross** is **Check Again**, **Circle** goes back. In a version with no online service address, the online question is left out.
It has been seen on a computer, not on a console. **Because it is offered only after a failed online try, a build made with `NP_ONLINE_URL=none` never shows it.** The screen has no **Save Report** button any more, so a support report is not written from the menu in this build
(see [Troubleshooting](#troubleshooting)).

## Languages

The multiplayer menus and messages follow the game's language: English, French, Spanish, German and Italian. The German and
Italian texts have not been checked by native speakers yet. Any other language shows English. Quick messages show in each
player's own language.

## Settings files

The game keeps your multiplayer settings in `ux0:data/smg/mp/mp.cfg`, a plain text file of `key = value` lines. The game writes
it for you. These keys are worth knowing:

| Key | What it does |
|---|---|
| `server = https://my-galaxy.example.org` | Use another online service than the game's own. See [Running your own online service](#running-your-own-online-service). |
| `stun = 0` | Never ask a public STUN server for your address. A far-away code then uses the router's address if the router gave one, else your local address. Default `1`. |
| `relay_only = 1` | Internet play through the relay only: your address is never shown to other players, and the Connection Test does not ask your router or a STUN server. `1` always wins; with `0` (default) the `SMG_NET_RELAY_ONLY` switch in `env.txt` decides (any value but `0` is on). |
| `name`, `name_typed`, `look`, `connection`, `mode`, `max_players`, `interaction`, `shared_kills`, `name_tags`, `rivals`, `race_wait`, `visibility`, `drop_in`, `downed`, `entry` | The choices the game remembers. `name` is your character's name (it follows the character you pick) unless `name_typed = 1`: then it is the name you wrote there and it is kept. The game never takes the PS Vita's user name. Most are no longer a menu choice (a game always has four places, bumping on, shared enemies and ghost racers; the gathering screen sets them). `downed` is your answer to the falls question. There is no menu for `name_tags` (0 off, 1 on, 2 on with an arrow at the screen edge); the default is on. |
| `console_key` | A secret random number made once on your console. It is never sent as it is: the game uses it to make the secret keys and tokens of each game. Do not share this file, and do not copy it to another console. |

Two small files sit beside it in `ux0:data/smg/mp/`: `flow.cfg` (your last choices in the menus, whether the falls question was asked (`falls_asked`), and the names, and friend codes where known, of the last 20 players you played with) and `coop.cfg`
(who played on each Team File). They are text files, they stay on your console, and nothing multiplayer writes touches your single-player saves.

Test switches for advanced users go in `ux0:data/smg/env.txt`: `SMG_MP_MENU=0` (no multiplayer menus), `SMG_MP_UPNP=0` (no UPnP),
`SMG_MP_NATPMP=0` (no NAT-PMP / PCP), `SMG_MP_DIRECT_PORT=<port>` (another port for far-away codes), `SMG_MP_STUN` (overrides the
`stun` key), `SMG_ATOMIC_SAVES=1` or `=0` (forces the safe single-player save on or off, over the Settings choice; see [Saves](#saves)), `SMG_MP_CRYPTO_LOG=1` (writes how long the game's encryption takes to `log.txt`; for testers). `SMG_NET_RELAY_ONLY=1` is the same as `relay_only = 1` in `mp.cfg` (see the table above for which one wins). The timing of how smooth the other players look (`SMG_MP_NET_SEND_MS`, `SMG_MP_NET_DELAY_MIN_MS`, `SMG_MP_NET_DELAY_MAX_MS`, `SMG_MP_NET_DELAY_EXTRA_MS`, `SMG_MP_NET_DELAY_PCT`, `SMG_MP_NET_EXTRAP_MS`, `SMG_MP_NET_KEEPALIVE_MS`, `SMG_MP_NET_BUNDLE_MS`, `SMG_MP_NET_HEARTBEAT_MS`, `SMG_MP_NET_POLL_MS`) and how often the `[mp] net:` statistics lines are written (`SMG_MP_NETLOG_SEC`, default 5 seconds, 0 = off) can be changed there too; the defaults, ranges and meanings are in [MULTIPLAYER-HARDWARE-TEST.md](MULTIPLAYER-HARDWARE-TEST.md).

### When the Vita is on Wi-Fi but cannot look for consoles on it

If the Vita is connected to Wi-Fi and the game still cannot open its way of finding consoles on that Wi-Fi, **Play Together** and
**Star Race** open the gathering screen anyway. You are alone on it. **Invite from Far Away** and **Play with Anyone** work as usual, and
the game tries the Wi-Fi again every three seconds; friends on the same Wi-Fi appear once it works. While it waits it asks nothing of
your router, a STUN server or the online service. (Before, this case showed "Couldn't connect to Wi-Fi", and kept showing it until the
game was restarted.) To get the error window instead, add `SMG_MP_GATHER_NOLAN=0` to `ux0:data/smg/env.txt`. This has not been seen
on a real PS Vita yet.

## Wi-Fi tip

Wi-Fi can pause when the console saves power, which makes play stutter. To keep play smooth, turn off Wi-Fi power
saving on your PS Vita:

1. Open **Settings**.
2. Choose **Power Save Settings**.
3. Turn off **Use Wi-Fi in Power Save Mode**.

The game shows this tip once, after it has noticed a weak or slow connection. While you play, the game also sends a small note to each player at least every
50 milliseconds so the Wi-Fi does not go to sleep. **This has been tried only in a simulation, never on a real Wi-Fi network.**

## Troubleshooting

Find the message you see, and read across. Windows show the text over two or three lines. The table gives it as one line.

| The message you see | What it means | What to do |
|---|---|---|
| Couldn't connect to Wi-Fi. Check your Wi-Fi settings. | The Vita is not on a Wi-Fi network. | Check your Wi-Fi settings and try again. Nearby play needs no router (turn Wi-Fi off). |
| Something went wrong. Please try again. (after choosing **Play Together** or **Star Race**) | The Vita is on Wi-Fi, but the game could not open what it needs to look for consoles on it. | Choose it again. If it keeps happening, restart the game. The game's log names the reason (a line that starts `gather: failed reason=`). |
| The online service isn't available right now. Please try again later. Friends in the same room can still play. | The online service did not answer, or your version of the game has no service address. | Online play is not open yet. Play on the same Wi-Fi, nearby, or with a far-away code. |
| Online play is busy right now. Please try again later. | The online service is using up its daily allowance and is not taking new games. | Wait. A game already going is not affected. Try again later or tomorrow (the day starts over at 00:00 UTC). |
| Online play is very busy today. Please try again tomorrow. Friends in the same room can still play. | A daily limit was reached. | Try again tomorrow. Wi-Fi, nearby and code games still work. |
| A newer version of the game is needed to play online. | The service only takes newer versions of the game. | Update the game. |
| A newer version of the game is available. | The service knows a newer version than yours. A small notice, once each time you start the game. | Nothing is wrong. Update when you can. Everyone in a game needs the same version. |
| That game couldn't be found. Check the code and try again. | No game has that code, or it has closed. | Check the code with your friend. |
| That code doesn't look right. Please check it and try again. | The code has a typing mistake. | **Cross** (OK) and the box opens again with your text in it. Fix it and send it again. |
| We couldn't reach your friend's game. Check the code and try again. Or give your friend a code instead. | The code was fine but your friend's console did not answer: the game is over, their router does not let it in, or your own Wi-Fi blocks it. | Check the code, then let your friend try the other way round: they choose **Invite from Far Away** and you type their code. |
| That game already has four players. Try again when someone leaves. | The game is full, or it is full for now: a place is held for a player whose connection dropped and who may come back. The game cannot say which. | Try again in a minute. |
| <name>'s Power Star couldn't be saved this time. Sorry! | You or another player play on a single-player file (**Play This File**) and a star was claimed that the game could not check against its own scene. Both players see it. | Nothing was written for that star; the rest of your file is as it was. |
| Your game and your friend's game are different versions. Please update both, then try again. | The two versions are not the same. | Make sure every player has the same version. |
| That game has already begun. Ask your friend to invite you again. | The game started before you got in. | Ask for a new code, or use **Play with <name>** on the gathering screen. |
| That took too long. Please try again. | The other console or the service did not answer in time. | Move closer to the router, or turn off power saving (see the Wi-Fi tip). |
| We couldn't reach that player from here. Try again, or play in the same room. | Neither a direct connection nor the relay worked from your network. | Try again. If you are in the same place, play on the same Wi-Fi. Or let the other player be the leader. |
| Check who you can play with? | The game offers its Connection Test after a connection fails. | Answer **Yes** to see what is wrong, **No** to go on. |
| Reconnecting... / Still trying to reconnect... | The link broke or the console slept. The game is putting you back in the same game. | Wait. It tries for 30 to 75 seconds (see [If you drop out](#if-you-drop-out)). |
| The connection was lost. You can go back in from the Multiplayer menu for two minutes. | The game could not put you back in time. | **Multiplayer**, then **Continue <name>'s Game**, within two minutes. |
| That game has ended. | You chose **Continue** but the game is over. | Start or join another game. |
| This game has ended. | The leader closed it. | Start or join another game. |
| The game went on without you. See you next time! | You were removed by the leader. | Start or join another game. |
| You were away for too long, so the game went on without you. | The online service dropped you from the game (more than a minute away). | Join again. |
| Your game data didn't match the other console's, so you left the game. Nothing on your console was changed. | Your save data did not agree with the leader's progress, so nothing was applied. | Tell the leader. Leave and join again. |
| Weak signal / <name>'s connection is weak. | Packets from that player are late. | Move closer to the router, or turn off power saving. |
| Everyone else left the game. / Everyone left. | You are the last one in the game. | Start or join a game again. |
| Please choose a file first. | No file has been chosen for the game. | Press **START** and answer **Which File?** |
| The save couldn't be prepared. | The chosen save could not be read or copied. | Choose another save. Your own file was not changed. |
| The progress couldn't be saved. | The Team File could not be written. | Pick another Team File. Check the free space on the memory card. |
| Your settings can't be saved right now. Nothing else changes. | The settings file is read only or could not be written. | Check the memory card. The game keeps working. |
| Your single-player file can't be opened right now. Your progress may not be saved. | Multiplayer could not read your single-player file. | Close the game and start it again. Your file was not changed. |
| Your file N has more Power Stars, or was saved more recently. Replace it anyway? / Team File N has more Power Stars... | You are about to replace newer progress. | Choose **No** unless you are sure. |
| Your friend can't be reached. Please check the code, or ask your friend to start the game instead. | Nobody answered at the code's address. | As for "We couldn't reach your friend's game". |
| Please set the date and time in the system settings. | The Vita's clock is far off, so the online service refuses it. | Set the date and time on the Vita. |
| That name can't be used. Please choose another one. | The service refuses the name (letters, numbers, spaces, dots, dashes and underscores only, and some words are not allowed). | Choose another name. |
| Too many tries in a short time. Please wait a moment and try again. | The service slows you down for a moment. | Wait a minute, then try again. |
| Online play isn't available for this account right now. | The service has blocked this account. | You can still play on the same Wi-Fi, nearby, or with a code. |
| Your friend list is full. Remove a friend to add another. | You have 50 friends. | Remove a friend first. |
| You can play with this player once you have added each other as friends. | They have not added you yet. | Ask them to add your friend code. |
| No player has that friend code. Check it and try again. | The code is wrong. | Check the code. |
| That's your own friend code. Share it with your friends! | You typed your own code. | Share it instead. |
| That friend code couldn't be added. / That didn't work. Try again later. | The service refused or did not answer. | Try again later. |
| Online play is busy right now. Please try again later. | The online service has used up what it can serve for now (a daily limit of its free hosting, or an overload). The game stops asking for a few minutes and keeps any race time it has not sent yet. | Try again in a few minutes, or tomorrow. Friends in the same room can still play. |
| Something went wrong. Please try again. | An unexpected problem. | Try again. If it keeps happening, send the log (see below). |

If the same problem comes back, send the file `ux0:data/smg/log.txt` with an
[issue report](https://github.com/CBOC-Vita-Lab/galaxy-vita/issues). **The log can show your game's network addresses, codes and names, so read it first and
take those out.** The game has code that writes a cleaned report file (`ux0:data/smg/report.txt`), but in this build no screen asks for it, so it is not made.

## Running your own online service

The online features need a server. The project's own service is not open to players yet (builds point at it by default, see the top of this guide), and a community can
run its own: the server is a Cloudflare Worker (free plan is enough for a small group) in
the `server/` folder of this repository ([server/README.md](../server/README.md), "Galaxy service"; deploy steps in `server/DEPLOY-SMG.txt`). To make the game use it, add one line to the multiplayer settings file
`ux0:data/smg/mp/mp.cfg` on every console of the group:

    server = https://my-galaxy.example.org

- HTTPS only; the address is just the host (and a port if it is not 443): no path, no spaces, at most 120 characters. A line that does not fit is ignored.
- The server's certificate must chain to one of the root certificates built into the game (Google Trust Services GTS Root R1 / R4, Let's Encrypt ISRG Root X1 / X2, SSL.com). `workers.dev` addresses and Cloudflare custom
  domains (the product's own address is https://galaxy.psvita.dev) do. A self-signed server is refused; verification cannot be turned off.
- Everyone in a game must use the same server and the same game version.
- Your online account (name, friend code, token) belongs to one server. The first time you use a new server the game makes an account there with the same name (a small puzzle your console solves first, to make mass sign-ups costly); your account on the other server stays where it is, and the token of
  one server is never sent to another. Delete the `server` line to go back to the default setting.
- The older switch still works for testing: `SMG_ONLINE_URL=https://...` in `ux0:data/smg/env.txt` (it wins over `mp.cfg`).

## Privacy

What the service keeps, what other players can see, and how to stay local-only are in [PRIVACY.md](PRIVACY.md).
