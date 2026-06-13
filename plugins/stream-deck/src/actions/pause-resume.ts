import { action, KeyUpEvent, SingletonAction, WillAppearEvent } from '@elgato/streamdeck';
import { ipc } from '../plugin.js';

@action({ UUID: 'com.monolith.recorder.pause-resume' })
export class PauseResume extends SingletonAction {
    override async onWillAppear(ev: WillAppearEvent): Promise<void> {
        if (ev.action.isKey()) {
            await ev.action.setTitle('⏸ Pause');
        }
    }

    override async onKeyUp(ev: KeyUpEvent): Promise<void> {
        try {
            await ipc.request('pause_resume');
            const status = await ipc.getStatus();
            if (ev.action.isKey()) {
                await ev.action.setTitle(status?.paused ? '▶ Resume' : '⏸ Pause');
            }
            await ev.action.showOk();
        } catch {
            await ev.action.showAlert();
        }
    }
}
