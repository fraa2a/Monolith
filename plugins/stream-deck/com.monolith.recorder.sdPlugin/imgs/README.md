# Key Images

Place PNG images here for Stream Deck key display. Required files:

| File              | Size        | Purpose                          |
|-------------------|-------------|----------------------------------|
| save-replay.png   | 72 × 72 px  | Save Replay action (normal DPI)  |
| save-replay@2x.png| 144 × 144 px| Save Replay action (Retina/HiDPI)|
| record-idle.png   | 72 × 72 px  | Toggle Recording — idle state    |
| record-idle@2x.png| 144 × 144 px| Toggle Recording — idle (HiDPI)  |
| record-active.png | 72 × 72 px  | Toggle Recording — active state  |
| record-active@2x.png|144×144 px | Toggle Recording — active (HiDPI)|
| pause.png         | 72 × 72 px  | Pause/Resume action              |
| pause@2x.png      | 144 × 144 px| Pause/Resume action (HiDPI)      |

The manifest.json references `save-replay`, `record-idle`, and `pause` as default
action icons. Stream Deck will show a placeholder if the files are missing.
