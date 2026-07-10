import * as fs from 'fs';
import * as net from 'net';
import * as path from 'path';

const IPC_HOST = '127.0.0.1';
const IPC_PORT = 45991;
const RECONNECT_MS = 2000;
const REQUEST_TIMEOUT_MS = 3000;

// Written by the engine at startup (user-only ACL). Read fresh on every
// request since the engine may restart and regenerate it while the plugin
// stays connected.
function readToken(): string {
    const base = process.env.LOCALAPPDATA ?? path.join(process.env.USERPROFILE ?? '', 'AppData', 'Local');
    try {
        return fs.readFileSync(path.join(base, 'Monolith', 'ipc_token'), 'utf8').trim();
    } catch {
        return '';
    }
}

export interface RecordingStatus {
    recording: boolean;
    paused: boolean;
    replay_enabled: boolean;
    recording_enabled: boolean;
}

type Resolver = [(value: unknown) => void, (reason: unknown) => void];

export class IpcClient {
    private socket: net.Socket | null = null;
    private buf = '';
    private pending = new Map<number, Resolver>();
    private nextId = 1;
    private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
    private destroyed = false;

    connect(): void {
        this._connect();
    }

    private _connect(): void {
        if (this.destroyed) return;

        const s = new net.Socket();
        s.setTimeout(0);

        s.connect(IPC_PORT, IPC_HOST, () => {
            this.socket = s;
            if (this.reconnectTimer !== null) {
                clearTimeout(this.reconnectTimer);
                this.reconnectTimer = null;
            }
        });

        s.on('data', (chunk: Buffer) => {
            this.buf += chunk.toString('utf8');
            let nl: number;
            while ((nl = this.buf.indexOf('\n')) !== -1) {
                const line = this.buf.slice(0, nl);
                this.buf = this.buf.slice(nl + 1);
                this._onLine(line.trim());
            }
        });

        s.on('close', () => {
            this.socket = null;
            this._scheduleReconnect();
        });

        s.on('error', () => {
            // handled by 'close' — suppress unhandled error event
        });
    }

    private _scheduleReconnect(): void {
        if (this.destroyed || this.reconnectTimer !== null) return;
        this.reconnectTimer = setTimeout(() => {
            this.reconnectTimer = null;
            this._connect();
        }, RECONNECT_MS);
    }

    private _onLine(line: string): void {
        if (!line) return;
        try {
            const msg = JSON.parse(line) as { id?: number; result?: unknown; error?: unknown };
            if (typeof msg.id !== 'number') return;
            const handlers = this.pending.get(msg.id);
            if (!handlers) return;
            this.pending.delete(msg.id);
            if (msg.error !== undefined) {
                handlers[1](msg.error);
            } else {
                handlers[0](msg.result);
            }
        } catch {
            // malformed response — ignore
        }
    }

    request(method: string): Promise<unknown> {
        return new Promise<unknown>((resolve, reject) => {
            if (!this.socket) {
                reject(new Error('IPC: not connected'));
                return;
            }
            const id = this.nextId++;
            this.pending.set(id, [resolve, reject]);

            const payload = JSON.stringify({ jsonrpc: '2.0', id, method, token: readToken() }) + '\n';
            this.socket.write(payload, 'utf8');

            setTimeout(() => {
                if (this.pending.delete(id)) {
                    reject(new Error(`IPC: timeout waiting for ${method}`));
                }
            }, REQUEST_TIMEOUT_MS);
        });
    }

    async getStatus(): Promise<RecordingStatus | null> {
        try {
            return (await this.request('get_status')) as RecordingStatus;
        } catch {
            return null;
        }
    }

    isConnected(): boolean {
        return this.socket !== null;
    }

    destroy(): void {
        this.destroyed = true;
        if (this.reconnectTimer !== null) {
            clearTimeout(this.reconnectTimer);
            this.reconnectTimer = null;
        }
        this.socket?.destroy();
        this.socket = null;
    }
}
