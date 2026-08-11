import { useCallback, useEffect, useState } from "preact/hooks";
import { type Clip, type CollectionSummary, collectionsApi } from "../lib/api.ts";
import { Icon } from "../shell/icons.tsx";
import { COLLECTION_COLORS } from "./collections-view.tsx";

export function CollectionPicker({ clip, onClose, onChanged }: {
  clip: Clip;
  onClose: () => void;
  onChanged: () => void;
}) {
  const [collections, setCollections] = useState<CollectionSummary[]>([]);
  const [membership, setMembership] = useState<Set<number>>(new Set());
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [busyId, setBusyId] = useState<number | null>(null);
  const [creating, setCreating] = useState(false);
  const [name, setName] = useState("");
  const [color, setColor] = useState(COLLECTION_COLORS[0]);
  const [busy, setBusy] = useState(false);

  const load = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const list = await collectionsApi.list();
      const flags = await Promise.all(
        list.map(async (c) => {
          const items = await collectionsApi.clips(c.id);
          return items.some((item) => item.source === clip.source && item.id === clip.id);
        }),
      );
      setCollections(list);
      setMembership(new Set(list.filter((_, i) => flags[i]).map((c) => c.id)));
    } catch (err) {
      setError(String(err));
    } finally {
      setLoading(false);
    }
  }, [clip.source, clip.id]);

  useEffect(() => {
    load();
  }, [load]);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") onClose();
    };
    document.addEventListener("keydown", onKey, true);
    return () => document.removeEventListener("keydown", onKey, true);
  }, [onClose]);

  const toggle = async (c: CollectionSummary) => {
    if (busyId !== null) return;
    setBusyId(c.id);
    setError(null);
    const inCol = membership.has(c.id);
    const res = inCol
      ? await collectionsApi.removeClip(c.id, clip)
      : await collectionsApi.addClip(c.id, clip);
    setBusyId(null);
    if (res.ok) {
      setMembership((prev) => {
        const next = new Set(prev);
        if (inCol) next.delete(c.id);
        else next.add(c.id);
        return next;
      });
      onChanged();
    } else {
      setError(res.error ?? "Operation failed");
    }
  };

  const createAndAdd = async () => {
    const trimmed = name.trim();
    if (!trimmed || busy) return;
    setBusy(true);
    setError(null);
    try {
      const id = await collectionsApi.create(trimmed, color);
      const res = await collectionsApi.addClip(id, clip);
      if (!res.ok) throw new Error(res.error ?? "Failed to add clip");
      setName("");
      setCreating(false);
      onChanged();
      await load();
    } catch (err) {
      setError(String(err));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div class="modal-backdrop" onMouseDown={onClose}>
      <div class="modal" onMouseDown={(e) => e.stopPropagation()}>
        <h3 class="modal-title">Add to collection</h3>

        <div class="collection-picker">
          {loading
            ? <span class="loading-dots"><i /><i /><i /></span>
            : collections.length === 0
            ? <p class="modal-msg">No collections yet — create one below.</p>
            : collections.map((c) => {
              const inCol = membership.has(c.id);
              return (
                <div class="collection-picker-row" key={c.id}>
                  <span class="color-swatch" style={{ background: c.color }} />
                  <span class="collection-name">{c.name}</span>
                  <button
                    class={inCol ? "btn btn-toggle on" : "btn btn-toggle"}
                    disabled={busyId !== null}
                    onClick={() => toggle(c)}
                  >
                    {busyId === c.id
                      ? "…"
                      : inCol
                      ? <><Icon name="check" size={14} /> In collection</>
                      : "Add"}
                  </button>
                </div>
              );
            })}
        </div>

        {error && <p class="err">{error}</p>}

        {creating
          ? (
            <div style={{ marginTop: "14px" }}>
              <input
                class="input"
                autoFocus
                value={name}
                disabled={busy}
                placeholder="Collection name"
                onInput={(e) => setName((e.target as HTMLInputElement).value)}
                onKeyDown={(e) => {
                  e.stopPropagation();
                  if (e.key === "Enter") createAndAdd();
                }}
              />
              <div style={{ display: "flex", gap: "8px", margin: "10px 0 4px" }}>
                {COLLECTION_COLORS.map((c) => (
                  <button
                    key={c}
                    type="button"
                    class={`color-swatch ${c === color ? "selected" : ""}`}
                    style={{ background: c }}
                    title={c}
                    aria-label={`Color ${c}`}
                    aria-pressed={c === color}
                    disabled={busy}
                    onClick={() => setColor(c)}
                  >
                    {c === color && <Icon name="check" size={12} />}
                  </button>
                ))}
              </div>
              <div class="modal-actions">
                <button class="btn" disabled={busy} onClick={() => setCreating(false)}>Cancel</button>
                <button class="btn btn-primary" disabled={!name.trim() || busy} onClick={createAndAdd}>
                  {busy ? "Creating…" : "Create"}
                </button>
              </div>
            </div>
          )
          : (
            <button class="btn btn-toggle" onClick={() => setCreating(true)}>
              <Icon name="plus" size={14} /> New collection…
            </button>
          )}

        <div class="modal-actions">
          <button class="btn" onClick={onClose}>Close</button>
        </div>
      </div>
    </div>
  );
}