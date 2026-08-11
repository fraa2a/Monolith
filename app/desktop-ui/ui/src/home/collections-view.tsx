import { useEffect, useState } from "preact/hooks";
import { type CollectionSummary, collectionsApi } from "../lib/api.ts";
import { Icon } from "../shell/icons.tsx";
import { ConfirmDialog } from "./confirm-dialog.tsx";

export const COLLECTION_COLORS = [
  "#f59e0b",
  "#e5544b",
  "#b48ead",
  "#7aa2f7",
  "#9ece6a",
  "#e0af68",
  "#0db9d7",
  "#bb9af7",
];

// Shared name+color modal used for create and rename (collections view and
// collection detail). onSave resolves true when the operation succeeded and
// the modal should close; thrown errors are shown inline.
export function CollectionFormModal(
  {
    title,
    initialName = "",
    initialColor = COLLECTION_COLORS[0],
    submitLabel = "Create",
    onSave,
    onCancel,
  }: {
    title: string;
    initialName?: string;
    initialColor?: string;
    submitLabel?: string;
    onSave: (name: string, color: string) => Promise<boolean>;
    onCancel: () => void;
  },
) {
  const [name, setName] = useState(initialName);
  const [color, setColor] = useState(initialColor);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") onCancel();
    };
    document.addEventListener("keydown", onKey, true);
    return () => document.removeEventListener("keydown", onKey, true);
  }, [onCancel]);

  const save = async () => {
    const trimmed = name.trim();
    if (!trimmed || busy) return;
    setBusy(true);
    setError(null);
    try {
      const ok = await onSave(trimmed, color);
      if (ok) onCancel();
    } catch (err) {
      setError(String(err));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div class="modal-backdrop" onMouseDown={onCancel}>
      <div class="modal" onMouseDown={(e) => e.stopPropagation()}>
        <h3 class="modal-title">{title}</h3>
        <input
          class="input"
          autoFocus
          value={name}
          disabled={busy}
          placeholder="Collection name"
          onInput={(e) => setName((e.target as HTMLInputElement).value)}
          onKeyDown={(e) => {
            e.stopPropagation();
            if (e.key === "Enter") save();
          }}
        />
        <div style={{ display: "flex", gap: "8px", margin: "12px 0 4px" }}>
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
        {error && <p class="err">{error}</p>}
        <div class="modal-actions">
          <button class="btn" disabled={busy} onClick={onCancel}>Cancel</button>
          <button class="btn btn-primary" disabled={!name.trim() || busy} onClick={save}>
            {busy ? "Saving…" : submitLabel}
          </button>
        </div>
      </div>
    </div>
  );
}

type CollectionFormState =
  | { mode: "create" }
  | { mode: "rename"; collection: CollectionSummary };

export function CollectionsView({ collections, onOpen, onChanged }: {
  collections: CollectionSummary[];
  onOpen: (id: number) => void;
  onChanged: () => void;
}) {
  const [form, setForm] = useState<CollectionFormState | null>(null);
  const [deleting, setDeleting] = useState<CollectionSummary | null>(null);

  return (
    <>
      <div class="grid-head">
        <h1 style={{ margin: 0, fontSize: "15px", fontWeight: 700 }}>Collections</h1>
        <span class="grid-count">
          <b>{collections.length}</b> {collections.length === 1 ? "collection" : "collections"}
        </span>
        <div class="rule" />
        <button class="btn btn-primary" onClick={() => setForm({ mode: "create" })}>
          New collection
        </button>
      </div>

      {collections.length === 0
        ? (
          <div class="empty">
            <div class="empty-glyph"><Icon name="album" size={26} /></div>
            <div class="empty-title">No collections yet</div>
            <div class="empty-hint">Right-click a clip → Add to collection.</div>
          </div>
        )
        : (
          <div class="collections-grid">
            {collections.map((c) => (
              <div
                key={c.id}
                class="collection-card"
                role="button"
                tabIndex={0}
                style={{ borderLeftColor: c.color }}
                onClick={() => onOpen(c.id)}
                onKeyDown={(e) => {
                  if (e.key === "Enter" || e.key === " ") {
                    e.preventDefault();
                    onOpen(c.id);
                  }
                }}
              >
                <div class="collection-cover">
                  {c.clip_count === 0
                    ? <Icon name="album" size={30} />
                    : <span class="color-swatch" style={{ background: c.color }} />}
                </div>
                <div class="collection-name">{c.name}</div>
                <div class="collection-count">
                  {c.clip_count} {c.clip_count === 1 ? "clip" : "clips"}
                </div>
                <button
                  class="card-act"
                  title="Rename"
                  onClick={(e) => {
                    e.stopPropagation();
                    setForm({ mode: "rename", collection: c });
                  }}
                >
                  <Icon name="pencil" size={14} />
                </button>
                <button
                  class="card-act danger"
                  title="Delete"
                  onClick={(e) => {
                    e.stopPropagation();
                    setDeleting(c);
                  }}
                >
                  <Icon name="trash-2" size={14} />
                </button>
              </div>
            ))}
          </div>
        )}

      {form && (
        <CollectionFormModal
          key={form.mode === "create" ? "create" : form.collection.id}
          title={form.mode === "create" ? "New collection" : "Rename collection"}
          submitLabel={form.mode === "create" ? "Create" : "Save"}
          initialName={form.mode === "rename" ? form.collection.name : ""}
          initialColor={form.mode === "rename" ? form.collection.color : COLLECTION_COLORS[0]}
          onSave={async (name, color) => {
            if (form.mode === "create") {
              await collectionsApi.create(name, color);
            } else {
              const res = await collectionsApi.rename(form.collection.id, name);
              if (!res.ok) throw new Error(res.error ?? "Rename failed");
            }
            onChanged();
            return true;
          }}
          onCancel={() => setForm(null)}
        />
      )}

      {deleting && (
        <ConfirmDialog
          title="Delete collection"
          message={`Delete "${deleting.name}"? This removes the collection but not the clips.`}
          confirmLabel="Delete"
          danger
          onConfirm={async () => {
            const res = await collectionsApi.remove(deleting.id);
            setDeleting(null);
            if (res.ok) onChanged();
          }}
          onCancel={() => setDeleting(null)}
        />
      )}
    </>
  );
}