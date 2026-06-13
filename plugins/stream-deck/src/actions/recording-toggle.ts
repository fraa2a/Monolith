import { action, KeyUpEvent, SingletonAction, WillAppearEvent } from '@elgato/streamdeck';
import { ipc } from '../plugin.js';

@action({ UUID: 'top.fraa2a.monolith.recording-toggle' })
export class RecordingToggle extends SingletonAction {
    override async onWillAppear(ev: WillAppearEvent): Promise<void> {
        if (ev.action.isKey()) {
            await ev.action.setTitle('⏺ REC');
        }
    }

    override async onKeyUp(ev: KeyUpEvent): Promise<void> {
        try {
            const status = await ipc.getStatus();
            if (status === null) {
                await ev.action.showAlert();
                return;
            }
            const method = status.recording ? 'recording_stop' : 'recording_start';
            await ipc.request(method);
            // Optimistic update; status polling corrects it within 5s.
            await ev.action.setTitle(status.recording ? '⏺ REC' : '⏹ STOP');
            await ev.action.showOk();
        } catch {
            await ev.action.showAlert();
        }
    }
}
