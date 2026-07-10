import * as fs from 'fs';
import * as net from 'net';
import * as path from 'path';
const IPC_HOST = '127.0.0.1';
const IPC_PORT = 45991;
const RECONNECT_MS = 2000;
const REQUEST_TIMEOUT_MS = 3000;
function readToken() {
    const base = process.env.LOCALAPPDATA ?? path.join(process.env.USERPROFILE ?? '', 'AppData', 'Local');
    try {
        return fs.readFileSync(path.join(base, 'Monolith', 'ipc_token'), 'utf8').trim();
    }
    catch {
        return '';
    }
}
export class IpcClient {
    socket = null;
    buf = '';
    pending = new Map();
    nextId = 1;
    reconnectTimer = null;
    destroyed = false;
    connect() {
        this._connect();
    }
    _connect() {
        if (this.destroyed)
            return;
        const s = new net.Socket();
        s.setTimeout(0);
        s.connect(IPC_PORT, IPC_HOST, () => {
            this.socket = s;
            if (this.reconnectTimer !== null) {
                clearTimeout(this.reconnectTimer);
                this.reconnectTimer = null;
            }
        });
        s.on('data', (chunk) => {
            this.buf += chunk.toString('utf8');
            let nl;
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
    _scheduleReconnect() {
        if (this.destroyed || this.reconnectTimer !== null)
            return;
        this.reconnectTimer = setTimeout(() => {
            this.reconnectTimer = null;
            this._connect();
        }, RECONNECT_MS);
    }
    _onLine(line) {
        if (!line)
            return;
        try {
            const msg = JSON.parse(line);
            if (typeof msg.id !== 'number')
                return;
            const handlers = this.pending.get(msg.id);
            if (!handlers)
                return;
            this.pending.delete(msg.id);
            if (msg.error !== undefined) {
                handlers[1](msg.error);
            }
            else {
                handlers[0](msg.result);
            }
        }
        catch {
            // malformed response — ignore
        }
    }
    request(method) {
        return new Promise((resolve, reject) => {
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
    async getStatus() {
        try {
            return (await this.request('get_status'));
        }
        catch {
            return null;
        }
    }
    isConnected() {
        return this.socket !== null;
    }
    destroy() {
        this.destroyed = true;
        if (this.reconnectTimer !== null) {
            clearTimeout(this.reconnectTimer);
            this.reconnectTimer = null;
        }
        this.socket?.destroy();
        this.socket = null;
    }
}
