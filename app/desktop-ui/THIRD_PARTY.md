# UI asset notices

## GLOW Icons

- Source: https://github.com/glow-ui/glow-icons
- Revision: `37d226bbcf129e19883f2c4436c749cc78169db9`
- License: MIT, copyright 2025 glow-ui.
- License file: `ui/public/licenses/glow-icons.txt` (copied to `dist/licenses` by Vite).

`ui/src/shell/icons.tsx` embeds the outline SVG subset used by Monolith and the
solid favorite star. The adapter uses `currentColor`, removes redundant white
fills and removes the video icon's viewport-only clip path to avoid repeated
SVG IDs. Icon names remain compatible with the existing callers. GLOW has no
gamepad icon in this revision; game controls use its grid symbol.

## Inter

- Project: https://github.com/rsms/inter
- License: SIL Open Font License 1.1, copyright 2016 The Inter Project Authors.
- Asset: `ui/fonts/InterVariable.woff2`.
- License file: `ui/public/licenses/inter-font.txt` (copied to `dist/licenses` by Vite).
- Imported without modification from `fraa2a/Legio`, revision
  `222afd3d56622d0514dbb1b666d27aedf62eb678`,
  `src/assets/fonts/InterVariable.woff2`.

The font and icons are bundled locally and require no remote asset service.

## Button design

`ui/src/components/ui/button.tsx` is a native Preact implementation of the
shadcn Button variant API and visual conventions: default, secondary, outline,
ghost, destructive, small and icon controls. Reference:
https://ui.shadcn.com/docs/components/button.
It uses the existing Preact runtime and plain CSS; it does not install the React,
Radix or Tailwind runtime stacks. Legacy `.btn` callers share the same CSS.
