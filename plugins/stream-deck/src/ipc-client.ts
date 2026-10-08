import * as net from 'net';

const IPC_HOST = '127.0.0.1';
const IPC_PORT = 45991;
const RECONNECT_MS = 2000;
const REQUEST_TIMEOUT_MS = 3000;

export interface RecordingStatus {
    recording: boolean;
    paused: boolean;
    replay_enabled: boolean;
    recording_enabled: boolean;
}

type Resolver = [(value: unknown) => void, (reason: unknown) => void, ReturnType<typeof setTimeout>];

export class IpcClient {
    private socket: net.Socket | null = null;
    private connected = false;
    private pending = new Map<number, Resolver>();
    private nextId = 1;
    private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
    private destroyed = false;

    connect(): void {
        this.openSocket();
    }

    private openSocket(): void {
        if (this.destroyed || this.socket) return;

        const s = new net.Socket();
        this.socket = s;
        s.setEncoding('utf8');
        let buf = '';

        s.connect(IPC_PORT, IPC_HOST, () => {
            this.connected = true;
            if (this.reconnectTimer !== null) {
                clearTimeout(this.reconnectTimer);
                this.reconnectTimer = null;
            }
        });

        s.on('data', (chunk: string) => {
            buf += chunk;
            let nl: number;
            while ((nl = buf.indexOf('\n')) !== -1) {
                const line = buf.slice(0, nl);
                buf = buf.slice(nl + 1);
                if (Buffer.byteLength(line) > 64 * 1024) { s.destroy(); return; }
                this.onLine(line.trim());
            }
            if (Buffer.byteLength(buf) > 64 * 1024) s.destroy();
        });

        s.on('close', () => {
            if (this.socket !== s) return;
            this.socket = null;
            this.connected = false;
            this.rejectPending(new Error('IPC: disconnected'));
            this.scheduleReconnect();
        });

        s.on('error', () => {
            // handled by 'close' - suppress unhandled error event
        });
    }

    private scheduleReconnect(): void {
        if (this.destroyed || this.reconnectTimer !== null) return;
        this.reconnectTimer = setTimeout(() => {
            this.reconnectTimer = null;
            this.openSocket();
        }, RECONNECT_MS);
    }

    private onLine(line: string): void {
        if (!line) return;
        try {
            const msg = JSON.parse(line) as { id?: number; result?: unknown; error?: unknown };
            if (!msg || typeof msg.id !== 'number') return;
            const handlers = this.pending.get(msg.id);
            if (!handlers) return;
            this.pending.delete(msg.id);
            clearTimeout(handlers[2]);
            if (msg.error !== undefined) {
                handlers[1](msg.error);
            } else {
                handlers[0](msg.result);
            }
        } catch {
            // malformed response - ignore
        }
    }

    request(method: string): Promise<unknown> {
        return new Promise<unknown>((resolve, reject) => {
            if (!this.connected || !this.socket) {
                reject(new Error('IPC: not connected'));
                return;
            }
            if (this.pending.size >= 64) {
                reject(new Error('IPC: too many pending requests'));
                return;
            }
            const id = this.nextId++;
            const timer = setTimeout(() => {
                if (this.pending.delete(id)) reject(new Error(`IPC: timeout waiting for ${method}`));
            }, REQUEST_TIMEOUT_MS);
            this.pending.set(id, [resolve, reject, timer]);

            const payload = JSON.stringify({ jsonrpc: '2.0', id, method }) + '\n';
            this.socket.write(payload, 'utf8');

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
        return this.connected;
    }

    private rejectPending(error: Error): void {
        for (const handlers of this.pending.values()) {
            clearTimeout(handlers[2]);
            handlers[1](error);
        }
        this.pending.clear();
    }

    destroy(): void {
        this.destroyed = true;
        if (this.reconnectTimer !== null) {
            clearTimeout(this.reconnectTimer);
            this.reconnectTimer = null;
        }
        this.socket?.destroy();
        this.socket = null;
        this.connected = false;
        this.rejectPending(new Error('IPC: client destroyed'));
    }
}
