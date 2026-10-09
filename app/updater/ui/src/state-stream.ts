export function attachState<T extends { revision: number }>(
  listen: (receive: (value: T) => void) => Promise<() => void>,
  snapshot: () => Promise<T>,
  receive: (value: T) => void,
): () => void {
  let disposed = false;
  let revision = -1;
  let unlisten: (() => void) | undefined;
  const accept = (value: T) => {
    if (disposed || !Number.isSafeInteger(value.revision) || value.revision <= revision) return;
    revision = value.revision;
    receive(value);
  };
  listen(accept).then(stop => {
    if (disposed) { stop(); return; }
    unlisten = stop;
    return snapshot().then(accept);
  }).catch(() => {});
  return () => { disposed = true; unlisten?.(); };
}
