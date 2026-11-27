// Galaxy (GAME=smg) per-star minimum plausible times: the table behind SMG_STAR_FLOORS.
//
// Keys are "<galaxy>:<star>" exactly as times.js looks them up: galaxy = the index in the client's race galaxy list (the Observatory
// order, decomp/src/Game/MP/MpMessages.cpp kGalaxies[], 0..41), star = the scenario number 1..8 (platform/src/mp/mp_service.cpp
// mp_race_submit). Values are frames at 60 per second (the unit of SMG_TIME_FLOOR_FRAMES and of the client's submissions).
//
// SOURCES (what could and could not be read; see also DEPLOY-SMG.txt "Star floors")
//  * SDA Knowledge Base, "Super Mario Galaxy/Star Times" (https://kb.speeddemosarchive.com/Super_Mario_Galaxy/Star_Times), read
//    8 Nov 2026. Fastest known times for single star missions, Mario and Luigi lists. Timing: from the moment the player gains control
//    until the Power Star is collected (the same span the race mode times: from the race GO until the star), in whole seconds for most
//    rows, so the data has +-1 s of noise. Its "Races" table uses the in-game timer. The page is OLD (community submissions of the
//    2008-2010 era, many rows "no run"); it is the only per-star list that could be fetched.
//  * speedrun.com (smg1 "Levels" and the IL leaderboards), the IL spreadsheets and MarioWiki could NOT be used: speedrun.com refuses
//    automated fetches (robots / client errors), the Google Sheets tabs do not load as text, and the MarioWiki has no per-star
//    record times. So no modern IL record was available. Modern ILs are certainly faster than these 2008-2010 SDA times.
//  * Galaxy names <-> stage names were read from this game's own message data (GalaxyName_* ids in UsEnglish Message.arc), not guessed;
//    e.g. BeltConveyerExGalaxy is Sweet Sweet Galaxy, TamakoroExLv2Galaxy is Rolling Gizmo Galaxy, TeresaMario2DGalaxy is Boo's Boneyard.
//
// FORMULA. floor = max(SMG_TIME_FLOOR_FRAMES (600), round(best_seconds * 60 * FLOOR_FACTOR)). The brief asked for about 85 % of the
// best known IL time. That is right for a current record list; for 15-year-old whole-second times it would reject honest modern
// runs (a record that is 30 % faster than the SDA row is plausible), and a wrongly rejected honest time is worse than one cheated time
// on a board that also flags every new best more than 25 % faster than the player's previous best. So FLOOR_FACTOR is 0.50 here
// (best = the faster of the Mario and Luigi rows, the Vita port runs the same game logic and physics, and a race that starts at the
// GO cannot be slower to reach the star than the same route from control). Raise it to 0.85 when a current speedrun.com IL table is
// available: put the new best times in BEST_SECONDS and change the factor; the tests follow the table.
//
// NO DATA. Every star not listed below keeps the global floor SMG_TIME_FLOOR_FRAMES (600 = 10 s): the shortest star in the whole SDA
// table is 20 s ("Luigi on the Roof"), so 10 s is below every real star. Galaxies with no row at all (floor 10 s for every star):
// see NO_DATA_GALAXIES.
//
// An operator can still override or add entries with the SMG_STAR_FLOORS variable (JSON), which wins over this table.

export const FLOOR_FACTOR = 0.5;
export const GLOBAL_FLOOR_FRAMES = 600;

/** index in the client's race galaxy list -> [stage name, English name] (checked against the game's own GalaxyName messages) */
export const GALAXIES = [
  ['EggStarGalaxy', 'Good Egg'], ['HoneyBeeKingdomGalaxy', 'Honeyhive'], ['SurfingLv1Galaxy', 'Loopdeeloop'],
  ['FlipPanelExGalaxy', 'Flipswitch'], ['TriLegLv1Galaxy', "Bowser Jr.'s Robot Reactor"], ['BeltConveyerExGalaxy', 'Sweet Sweet'],
  ['StarDustGalaxy', 'Space Junk'], ['BattleShipGalaxy', 'Battlerock'], ['TamakoroExLv1Galaxy', 'Rolling Green'],
  ['BreakDownPlanetGalaxy', 'Hurry-Scurry'], ['KoopaBattleVs1Galaxy', "Bowser's Star Reactor"], ['CocoonExGalaxy', 'Sling Pod'],
  ['HeavenlyBeachGalaxy', 'Beach Bowl'], ['PhantomGalaxy', 'Ghostly'], ['CubeBubbleExLv1Galaxy', 'Bubble Breeze'],
  ['OceanFloaterLandGalaxy', 'Buoy Base'], ['KoopaJrShipLv1Galaxy', "Bowser Jr.'s Airship Armada"], ['TearDropGalaxy', 'Drip Drop'],
  ['CosmosGardenGalaxy', 'Gusty Garden'], ['IceVolcanoGalaxy', 'Freezeflame'], ['SandClockGalaxy', 'Dusty Dune'],
  ['HoneyBeeExGalaxy', 'Honeyclimb'], ['KoopaBattleVs2Galaxy', "Bowser's Dark Matter Plant"], ['FishTunnelGalaxy', 'Bigmouth'],
  ['ReverseKingdomGalaxy', 'Gold Leaf'], ['OceanRingGalaxy', 'Sea Slide'], ['FactoryGalaxy', 'Toy Time'],
  ['SkullSharkGalaxy', 'Bonefin'], ['FloaterOtaKingGalaxy', "Bowser Jr.'s Lava Reactor"], ['TransformationExGalaxy', 'Sand Spiral'],
  ['OceanPhantomCaveGalaxy', 'Deep Dark'], ['CannonFleetGalaxy', 'Dreadnought'], ['HellProminenceGalaxy', 'Melty Molten'],
  ['DarkRoomGalaxy', 'Matter Splatter'], ['KoopaBattleVs3Galaxy', "Bowser's Galaxy Reactor"], ['SnowCapsuleGalaxy', 'Snow Cap'],
  ['HeavensDoorGalaxy', 'Gateway'], ['SurfingLv2Galaxy', 'Loopdeeswoop'], ['TamakoroExLv2Galaxy', 'Rolling Gizmo'],
  ['CubeBubbleExLv2Galaxy', 'Bubble Blast'], ['TeresaMario2DGalaxy', "Boo's Boneyard"], ['PeachCastleFinalGalaxy', 'Grand Finale'],
];

/**
 * Best known time in seconds per [galaxy index, scenario] (SDA Star Times: the faster of Mario and Luigi, "Races" rows are the
 * in-game timer). Scenario numbers follow the SDA list order of each galaxy, which is the in-game mission order.
 */
export const BEST_SECONDS = [
  // Good Egg
  [0, 2, 201.7], [0, 3, 122], [0, 4, 69], [0, 5, 82], [0, 6, 20],
  // Honeyhive
  [1, 1, 62], [1, 2, 60], [1, 3, 97], [1, 4, 32], [1, 6, 30],
  [2, 1, 55.6], // Loopdeeloop: Surfing 101 (Races table)
  [3, 1, 21], // Flipswitch: Painting the Planet Yellow
  [4, 1, 89], // Bowser Jr.'s Robot Reactor: Megaleg's Moon
  // Space Junk
  [6, 1, 97], [6, 2, 107], [6, 3, 119], [6, 4, 97], [6, 5, 37],
  // Battlerock
  [7, 2, 109], [7, 3, 137],
  [8, 1, 47], // Rolling Green: Rolling in the Clouds
  [10, 1, 169], // Bowser's Star Reactor: The Fiery Stronghold
  // Beach Bowl
  [12, 1, 22.8], [12, 2, 37], [12, 3, 59], [12, 4, 26], [12, 6, 59],
  // Ghostly
  [13, 1, 113], [13, 2, 37.63], [13, 3, 216], [13, 4, 92], [13, 6, 72],
  // Buoy Base
  [15, 1, 101], [15, 2, 42],
  [16, 1, 92], // Bowser Jr.'s Airship Armada: Sinking the Airships
  // Gusty Garden
  [18, 1, 66], [18, 2, 124], [18, 3, 109],
  [19, 1, 100], // Freezeflame: The Frozen Peak of Baron Brr
  // Dusty Dune
  [20, 1, 116], [20, 7, 82],
  [22, 1, 189], // Bowser's Dark Matter Plant: Darkness on the Horizon
  // Gold Leaf
  [24, 2, 104], [24, 3, 79], [24, 4, 39],
  // Sea Slide
  [25, 2, 46.4], [25, 4, 36],
  // Toy Time
  [26, 1, 155], [26, 5, 61],
  [28, 1, 94], // Bowser Jr.'s Lava Reactor: King Kaliente's Spicy Return
  // Melty Molten
  [32, 1, 144], [32, 2, 140], [32, 4, 154], [32, 5, 163],
  [34, 1, 356], // Bowser's Galaxy Reactor: The Fate of the Universe
  // Planet of Trials stars (scenario 1 of their galaxy)
  [37, 1, 47.38], // Loopdeeswoop: The Galaxy's Greatest Wave (Races table)
  [38, 1, 58], // Rolling Gizmo: Gizmos, Gears, and Gadgets
  [39, 1, 154.7], // Bubble Blast: The Electric Labyrinth
];

const galaxiesWithData = new Set(BEST_SECONDS.map((e) => e[0]));
/** galaxy indexes with no known time at all: every star there keeps the global floor */
export const NO_DATA_GALAXIES = GALAXIES.map((_, i) => i).filter((i) => !galaxiesWithData.has(i));

/** {"<galaxy>:<star>": frames}: the default value of SMG_STAR_FLOORS (never below the global floor). */
export function buildFloors(factor = FLOOR_FACTOR) {
  const out = {};
  for (const [g, s, sec] of BEST_SECONDS) {
    const frames = Math.max(GLOBAL_FLOOR_FRAMES, Math.round(sec * 60 * factor));
    if (frames > GLOBAL_FLOOR_FRAMES) out[`${g}:${s}`] = frames;
  }
  return out;
}

export const DEFAULT_STAR_FLOORS = Object.freeze(buildFloors());
