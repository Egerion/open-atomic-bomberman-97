# HD artwork overrides

The game preserves the original 640x480 gameplay coordinate system and all
simulation rules. Press **Tab** at any time to switch between classic artwork
and optional HD overrides; the switch is presentation-only and instantaneous.

Place a replacement asset at the same relative path under `DATA_HD`:

```
<game install>/DATA_HD/RES/TITLE.PCX
<game install>/DATA_HD/RES/MAINMENU.PCX
<game install>/DATA_HD/RES/FIELD0.PCX
```

The override loader accepts the original indexed PCX format and standard
24-bit RGB PCX. Use 4:3 images at an integer multiple of the original size:

- full-screen artwork: 2560x1920 for the original 640x480 screens;
- small UI art: 4x the original dimensions;
- do not replace `DATA`; it remains the permanent fallback.

An absent, malformed, or not-yet-authored HD file falls back to the matching
original resource, so a partially completed art pack is always playable.
The initial HD layer covers front-end PCX artwork and stage `FIELD<n>`
backgrounds. ANI character, bomb, tile, and effect replacement needs a
frame-manifest layer and is intentionally kept separate from this safe first
step.
