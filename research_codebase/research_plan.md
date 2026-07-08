# Research Plan: Monolith Codebase Documentation Update

## Main Question

What does the current Monolith codebase actually implement, and how should all
Markdown documentation be updated so it matches the repository state?

## Subtopics

1. Native engine architecture
   - Expected information: modules, runtime ownership, capture/audio/encode
     paths, settings reload, IPC, updater.

2. Desktop UI architecture and UX
   - Expected information: current UI shell, frontend stack, data access model,
     accessibility/interaction risks.

3. Stream Deck plugin state
   - Expected information: implemented actions, manifest IDs, IPC methods,
     build/package commands, asset gaps.

4. Documentation drift
   - Expected information: stale references to WinUI, Deno Desktop, config.json,
     unimplemented plugin, old active-game settings.

## Synthesis

Findings are synthesized into `research_report.md` and reflected in every
existing `.md` documentation file in the repository.

## Source Policy

This task is a local repository audit. External web sources were not needed; the
primary sources are local code, manifests, build files, and existing docs.
