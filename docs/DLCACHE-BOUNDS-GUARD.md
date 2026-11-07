# Display-list cache bounds guard

`GXV_DLCACHE_BOUNDS_GUARD=1` is an opt-in correctness guard. It is read once
with `EnvInt` when the display-list cache initializes and defaults to OFF.
It does not change simulation, source geometry, out-of-range fallback values,
or valid-list cache behavior.

## Failure and scope

The vertex decoder substitutes zeros for indexed elements outside the current
array binding. `MeasureArraySpans` records only readable elements. Previously,
the geometry cache could store those fallback zeros while omitting the failed
reads from its binding identity and content hash. A later valid binding or
expanded readable range could therefore keep hitting stale geometry, even in
`GXV_DLCACHE=verify` mode.

With the guard ON, the existing index scan also reports whether every indexed
element the decoder consumes is readable. An incomplete decode is refused by
the cache and follows the existing uncached parser/bypass path. No second scan
or additional cache-entry metadata is needed. With the guard OFF, completeness
is not requested and the original path remains available for comparison.

A previously complete cache entry cannot be used while its required range no
longer fits: `SameState` already compares its highest read byte with the current
array size. The replacement partial entry is now refused. An older complete
variant may remain stored, but the bypass uses the current parser; after the
bypass expires, normal generation/hash validation precedes reuse of that older
variant. Content changes in formerly unreadable elements are therefore checked.
This retains the existing guest-write notification contract: normal cache mode
does not hash unnotified mutations on every hit, while verify mode does.

Refusal retains the existing 600-frame bypass policy. This favors correctness
over cache reuse after malformed bindings. It does not establish that an array
pointer is physically valid beyond the bounds supplied by the guest resolver.

## Host verification

Run `bash tools/test_vgx_dlcache_bounds_guard.sh`.

The test uses synthetic registered RAM and actual CP writes, planner, parser,
geometry cache, command emitter, static buffers, and null backend. It checks
every emitted position component against independently specified expectations.

- Unmapped binding followed by valid binding.
- Partial readable range followed by bounds expansion.
- Warmed/promoted valid entry, bounds shrink, then recovery with edits only to
  formerly unreadable elements, running 610 frames across bypass expiry.
- Fully valid rebind, unchanged valid indexed lists, and direct vertices.
- Cache OFF/ON/verify; static buffers OFF/ON; diagnostics OFF/ON.
- Unset versus explicit OFF exact output parity, including legacy geometry.
- Read-once behavior when the environment changes after a valid priming build.

The full suite runs 288 ASan/UBSan process cases. OFF-path cases deliberately
assert legacy behavior; they are parity controls, not claims that stale fallback
geometry is correct. The opt-in recovery case failed before the guard and passes
with it. These are local correctness tests, not physical-Vita gameplay,
performance, or proof of the cause of a reported transition failure.
