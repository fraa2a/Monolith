import * as net from 'net';
const IPC_HOST = '127.0.0.1';
const IPC_PORT = 45991;
const RECONNECT_MS = 2000;
const REQUEST_TIMEOUT_MS = 3000;
export class IpcClient {
    socket = null;
    connected = false;
    pending = new Map();
    nextId = 1;
    reconnectTimer = null;
    destroyed = false;
    connect() {
        this.openSocket();
    }
    openSocket() {
        if (this.destroyed || this.socket)
            return;
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
        s.on('data', (chunk) => {
            buf += chunk;
            let nl;
            while ((nl = buf.indexOf('\n')) !== -1) {
                const line = buf.slice(0, nl);
                buf = buf.slice(nl + 1);
                if (Buffer.byteLength(line) > 64 * 1024) {
                    s.destroy();
                    return;
                }
                this.onLine(line.trim());
            }
            if (Buffer.byteLength(buf) > 64 * 1024)
                s.destroy();
        });
        s.on('close', () => {
            if (this.socket !== s)
                return;
            this.socket = null;
            this.connected = false;
            this.rejectPending(new Error('IPC: disconnected'));
            this.scheduleReconnect();
        });
        s.on('error', () => {
            // handled by 'close' - suppress unhandled error event
        });
    }
    scheduleReconnect() {
        if (this.destroyed || this.reconnectTimer !== null)
            return;
        this.reconnectTimer = setTimeout(() => {
            this.reconnectTimer = null;
            this.openSocket();
        }, RECONNECT_MS);
    }
    onLine(line) {
        if (!line)
            return;
        try {
            const msg = JSON.parse(line);
            if (!msg || typeof msg.id !== 'number')
                return;
            const handlers = this.pending.get(msg.id);
            if (!handlers)
                return;
            this.pending.delete(msg.id);
            clearTimeout(handlers[2]);
            if (msg.error !== undefined) {
                handlers[1](msg.error);
            }
            else {
                handlers[0](msg.result);
            }
        }
        catch {
            // malformed response - ignore
        }
    }
    request(method) {
        return new Promise((resolve, reject) => {
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
                if (this.pending.delete(id))
                    reject(new Error(`IPC: timeout waiting for ${method}`));
            }, REQUEST_TIMEOUT_MS);
            this.pending.set(id, [resolve, reject, timer]);
            const payload = JSON.stringify({ jsonrpc: '2.0', id, method }) + '\n';
            this.socket.write(payload, 'utf8');
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
        return this.connected;
    }
    rejectPending(error) {
        for (const handlers of this.pending.values()) {
            clearTimeout(handlers[2]);
            handlers[1](error);
        }
        this.pending.clear();
    }
    destroy() {
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
