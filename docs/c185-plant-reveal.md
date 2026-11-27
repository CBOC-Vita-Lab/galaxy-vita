# Honeyhive plant reappearance

The control census caught grass groups changing visibility in the middle of the
opening camera (approximately 100 and 135 pixels across their bounding spheres)
and a blue-flower group later in the route. Increasing the groups' draw distance
would also start their sensor, effects and movement early. C185 leaves that
distance and the clip/unclip callbacks alone.

On the active Performance decor-budget return of a neutral Honeyhive CutBushGroup
or FlowerBlueGroup, at least half of its drawable members appear in the original
native picture. The others join on the next native picture. Existing J3D packet
cull stamps handle both live and frozen lists; there is no second pass, blending,
duplicate geometry or gameplay timer. This softens a single group-wide reveal,
not every scenery pop. It cannot make plants visible before the original unclip.

The group must have 3-7 authored members, at least 3 eligible models, no
authored items, and no current or pending member thinning. The camera and player
must both be at least 900 units beyond the group radius. Missing player, other
stages, unaffected categories and the native/reference clip path are untouched.
The work is bounded to seven checks on an actual reappearance. Up to three fewer
models are submitted on the first picture, none on subsequent pictures relative
to control. There is no measured steady-state FPS saving from this change.

## Verification

Run `bash tools/test_c185_plant.sh`. For the pixel comparison, pass the private
cooked game `files/ObjectData` directory and optionally an output PNG. Set
`PYTHON` to an interpreter with NumPy and Pillow. No game data are packaged.
The fixture maps the authored texture onto each authored mesh in a group ring.
It tests two distinct native pictures using the production visibility mask and
checks full pixel recovery. It is a static projection, not a game screenshot.

For runtime comparison, run the same HoneyBeeKingdomGalaxy scenario 1 save with
the same Performance options and `GXV_POPIN_CENSUS=1` in each fresh run. Compare
the opening camera and the later no-contact blue-flowers pan with
`GXV_HONEY_PLANT_REVEAL=0` versus `1` (default). The candidate logging build
prints up to 64 `honey-plant:` records: picture, family, world coordinates,
eligible models and first/delayed counts. These are packet decisions, not proof
that a model's pixels survived occlusion. Match the same camera position and
check the first and next **real** pictures rather than adjacent display slots;
the first should already have plants, the second should recover all visible
members. Check a nearby group, an item-bearing group, and a second stage as
unchanged controls. Generated midpoint images require separate acceptance.
