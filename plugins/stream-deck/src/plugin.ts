import streamDeck from '@elgato/streamdeck';

import { IpcClient } from './ipc-client.js';
import { SaveReplay } from './actions/save-replay.js';
import { RecordingToggle } from './actions/recording-toggle.js';
import { PauseResume } from './actions/pause-resume.js';

// Shared IPC client — actions import this directly.
export const ipc = new IpcClient();
ipc.connect();

// Register all actions.
streamDeck.actions.registerAction(new SaveReplay());
streamDeck.actions.registerAction(new RecordingToggle());
streamDeck.actions.registerAction(new PauseResume());

// Poll recording state every 5s and push title updates to all active action
// instances so the key labels stay in sync even without user interaction.
setInterval(async () => {
    const status = await ipc.getStatus();
    if (status === null) return;

    for (const action of streamDeck.actions) {
        if (!action.isKey()) continue;
        try {
            if (action.manifestId === 'com.monolith.recorder.recording-toggle') {
                await action.setTitle(status.recording ? '⏹ STOP' : '⏺ REC');
            } else if (action.manifestId === 'com.monolith.recorder.pause-resume') {
                await action.setTitle(status.paused ? '▶ Resume' : '⏸ Pause');
            }
        } catch {
            // action may have disappeared — skip
        }
    }
}, 5000);

// Connect to the Stream Deck software and start the plugin.
await streamDeck.connect();
