import test from 'node:test';
import assert from 'node:assert/strict';
import { attachState } from '../../app/updater/ui/src/state-stream.ts';

test('listener precedes snapshot and late snapshots cannot replace terminal events', async () => {
  let event;
  let resolveSnapshot;
  let detached = false;
  const seen = [];
  const stop = attachState(
    async receive => { event = receive; return () => { detached = true; }; },
    () => { assert.ok(event); return new Promise(resolve => { resolveSnapshot = resolve; }); },
    value => seen.push(value.phase),
  );
  await Promise.resolve();
  event({ revision: 3, phase: 'done' });
  resolveSnapshot({ revision: 2, phase: 'downloading' });
  await Promise.resolve();
  await Promise.resolve();
  assert.deepEqual(seen, ['done']);
  event({ revision: 1, phase: 'available' });
  assert.deepEqual(seen, ['done']);
  stop();
  assert.equal(detached, true);
  event({ revision: 4, phase: 'failed' });
  assert.deepEqual(seen, ['done']);
});

test('unmount before listener registration cleans up without reading snapshot', async () => {
  let resolveListener;
  let detached = false;
  const stop = attachState(
    () => new Promise(resolve => { resolveListener = resolve; }),
    () => { throw new Error('snapshot after disposal'); },
    () => { throw new Error('event after disposal'); },
  );
  stop();
  resolveListener(() => { detached = true; });
  await Promise.resolve();
  assert.equal(detached, true);
});
