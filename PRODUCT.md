# Product

## Register

product

## Users

PC gamers and desktop users on Windows 11 who keep a replay buffer running while
they play. They interact with Monolith in short bursts: glance at capture
status, save a moment, browse and tag clips afterwards. The recorder itself is
background software; the UI is a library and control surface, not a workspace
they live in.

## Product Purpose

Monolith is a recorder-first screen capture and replay-buffer app. It exists so
a moment can be saved after it happens (Ctrl+Shift+F8) and found again quickly.
Success: capture state is always legible at a glance, saved clips appear
instantly in the library, and the app never gets in the way of the game.

## Brand Personality

Quiet, precise, native. The app should feel like part of Windows: dark chrome,
monochrome restraint, red reserved for recording/destructive meaning. No
gamer-RGB styling, no streamer-brand loudness.

## Anti-references

- OBS-style density: no wall of technical toggles on primary surfaces.
- GeForce Experience overlay chrome: no neon accents or promotional panels.
- Debug aesthetics: raw process names, device IDs, file paths, or engine
  jargon are never user-facing copy.

## Design Principles

1. Status is ambient: recording/clipping state readable in under a second,
   without opening anything.
2. Resolve, don't expose: every process, device, or file is shown by its
   human name and icon; identifiers stay internal.
3. One accent, one meaning: red means recording or destruction, nothing else.
4. Native fidelity: Windows-resident fonts, standard affordances, offline and
   CSP-clean assets.
5. The game wins: the UI never demands attention while capture is running.

## Accessibility & Inclusion

- Dark-only UI today; maintain ≥4.5:1 body-text contrast against chrome and
  surfaces.
- All interactive controls need visible hover/disabled states and tooltips;
  status must not be conveyed by color alone (pair dot color with a text label).
- Honor `prefers-reduced-motion` for any pulse/entrance animation.
