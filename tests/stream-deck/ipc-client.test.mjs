import assert from 'node:assert/strict';
import net from 'node:net';
import { once } from 'node:events';
import { setTimeout as delay } from 'node:timers/promises';
import test from 'node:test';
import { IpcClient } from '../../plugins/stream-deck/.build/top.fraa2a.monolith.sdPlugin/bin/ipc-client.js';

async function until(predicate) {
  for (let attempt = 0; attempt < 100; attempt++) {
    if (predicate()) return;
    await delay(10);
  }
  throw new Error('condition timed out');
}

test('IPC handles UTF-8 splits, disconnects, response limits and shutdown', { timeout: 15000 }, async () => {
  let connections = 0;
  let peer;
  const peers = new Set();
  const server = net.createServer(socket => {
    peer = socket;
    peers.add(socket);
    socket.on('close', () => peers.delete(socket));
    connections++;
  });
  server.listen(45991, '127.0.0.1');
  await once(server, 'listening');
  const client = new IpcClient();
  try {
    client.connect(); client.connect();
    await until(() => client.isConnected() && peer);
    assert.equal(connections, 1);
    const data = once(peer, 'data');
    const reply = client.request('get_status');
    const request = JSON.parse((await data)[0].toString());
    const response = Buffer.from(JSON.stringify({ jsonrpc: '2.0', id: request.id, result: 'caffè' }) + '\n');
    const split = response.indexOf(Buffer.from('è')) + 1;
    peer.write(response.subarray(0, split));
    await delay(10);
    peer.write(response.subarray(split));
    assert.equal(await reply, 'caffè');
    peer.write('{"id":');
    const pending = client.request('get_status');
    const rejected = assert.rejects(pending, /disconnected/);
    peer.destroy();
    await rejected;
    await until(() => !client.isConnected());
    client.connect();
    await until(() => client.isConnected() && connections === 2);
    const nextData = once(peer, 'data');
    const next = client.request('get_status');
    const nextRequest = JSON.parse((await nextData)[0].toString());
    peer.write(JSON.stringify({ jsonrpc: '2.0', id: nextRequest.id, result: 42 }) + '\n');
    assert.equal(await next, 42);
    peer.write('x'.repeat(65537));
    await until(() => !client.isConnected());
    client.connect(); await until(() => client.isConnected() && connections === 3);
    const waiting = Array.from({ length: 64 }, () => client.request('get_status').catch(error => error));
    await assert.rejects(client.request('get_status'), /too many pending/);
    client.destroy();
    for (const result of await Promise.all(waiting)) assert.match(result.message, /destroyed/);
    await assert.rejects(client.request('get_status'), /not connected/);
    client.connect(); await delay(20);
    assert.equal(connections, 3);
  } finally {
    client.destroy();
    for (const socket of peers) socket.destroy();
    server.close();
    await once(server, 'close');
  }
});


test('IPC rejects malformed envelopes and status objects', async () => {
  let peer;
  const server = net.createServer(socket => { peer = socket; });
  server.listen(45991, '127.0.0.1');
  await once(server, 'listening');
  const client = new IpcClient();
  try {
    client.connect();
    await until(() => client.isConnected() && peer);
    for (const fields of [{}, { result: null, error: {} }, { jsonrpc: '1.0', result: true }]) {
      const data = once(peer, 'data');
      const reply = client.request('save_replay');
      const rejected = assert.rejects(reply, /invalid.*response/);
      const request = JSON.parse((await data)[0].toString());
      peer.write(JSON.stringify({ jsonrpc: '2.0', id: request.id, ...fields }) + '\n');
      await rejected;
    }
    for (const result of [null, {}, { recording: true }, { recording: true, paused: false, replay_enabled: true, recording_enabled: 'yes' }]) {
      const data = once(peer, 'data');
      const reply = client.getStatus();
      const request = JSON.parse((await data)[0].toString());
      peer.write(JSON.stringify({ jsonrpc: '2.0', id: request.id, result }) + '\n');
      assert.equal(await reply, null);
    }
  } finally {
    client.destroy(); peer?.destroy(); server.close(); await once(server, 'close');
  }
});
