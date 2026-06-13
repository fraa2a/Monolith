import { action, KeyUpEvent, SingletonAction, WillAppearEvent } from '@elgato/streamdeck';
import { ipc } from '../plugin.js';

@action({ UUID: 'com.monolith.recorder.save-replay' })
export class SaveReplay extends SingletonAction {
    override async onWillAppear(ev: WillAppearEvent): Promise<void> {
        if (ev.action.isKey()) {
            await ev.action.setTitle('Save\nReplay');
        }
    }

    override async onKeyUp(ev: KeyUpEvent): Promise<void> {
        try {
            await ipc.request('save_replay');
            await ev.action.showOk();
        } catch {
            await ev.action.showAlert();
        }
    }
}
