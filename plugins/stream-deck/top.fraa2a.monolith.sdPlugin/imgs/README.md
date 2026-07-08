# Stream Deck Images

Place PNG images here for the packaged Stream Deck plugin.

The manifest references these asset stems:

| Stem | Purpose |
|---|---|
| `plugin` | Plugin icon |
| `category` | Category icon |
| `save-replay` | Action icon |
| `save-replay-key` | Save Replay key state |
| `record-idle` | Toggle Recording action icon |
| `record-idle-key` | Recording idle key state |
| `record-active` | Recording active state, if added later |
| `pause` | Pause/Resume action icon |
| `pause-key` | Pause key state |

Recommended sizes:

- 72 x 72 px for normal key images.
- 144 x 144 px for HiDPI key images if separate `@2x` assets are added.
- PNG with transparent background.

The current manifest does not list every legacy `@2x` filename that older notes
mentioned. Keep this README and `manifest.json` in sync when asset names change.
