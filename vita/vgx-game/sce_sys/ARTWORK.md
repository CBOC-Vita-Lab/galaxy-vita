# Galaxy 3.0 Bubble & LiveArea Artwork

On 2026-11-26 the four packaged PS Vita bubble and LiveArea PNGs (`icon0.png`, `pic0.png`,
`livearea/contents/bg.png`, `livearea/contents/startup.png`) were upgraded for Galaxy 3.0
(and `template.xml` bumped to `content-rev="5"` so existing PS Vita installs refresh the LiveArea cache):

- `icon0.png` (128×128, 8-bit 256-colour palette PNG, opaque): circular-safe Home Screen bubble framing Mario, Luma, cosmic spiral halo, and Star Bits inside the Vita's round icon mask.
- `livearea/contents/bg.png` (840×500, 8-bit 256-colour palette PNG, opaque): LiveArea background framed specifically around the `a1` Start gate so Mario + Luma sit on the left, the Comet Observatory + Good Egg planet sit on the right, and the center-bottom cosmic pocket stays open behind the Start button.
- `livearea/contents/startup.png` (280×158, 8-bit 256-colour palette PNG with `tRNS` palette alpha): crisp 3D remastered Super Mario Galaxy emblem with anti-aliased transparent silhouette.
- `pic0.png` (960×544, 8-bit 256-colour palette PNG, opaque): full-screen launch splash matching the LiveArea art direction with the remastered title emblem.

Legacy note: `livearea/contents/settings.png` (Contributor artwork from 2026-11-17) remains in the tree untouched and is not packaged into the VPK (Settings live inside the game). Do not accidentally regenerate or overwrite these approved PNGs with neutral fallback art.
