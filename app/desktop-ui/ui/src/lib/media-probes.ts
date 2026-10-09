type Probe = { run: () => Promise<void>; cancelled: boolean };
const pending: Probe[] = [];
let active = 0;
const LIMIT = 2;

function drain(): void {
  while (active < LIMIT && pending.length) {
    const probe = pending.shift()!;
    if (probe.cancelled) continue;
    ++active;
    void probe.run().catch((error) => console.error("Media probe failed", error)).finally(() => {
      --active;
      drain();
    });
  }
}

export function enqueueMediaProbe(run: () => Promise<void>): () => void {
  const probe = { run, cancelled: false };
  pending.push(probe);
  drain();
  return () => { probe.cancelled = true; };
}
