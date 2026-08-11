import { useCallback, useEffect, useRef, useState } from "preact/hooks";
import { type Clip, type CollectionSummary, clipApi, collectionsApi } from "../lib/api.ts";
import { Icon } from "../shell/icons.tsx";
import { ClipCard } from "./clip-card.tsx";
import { ConfirmDialog } from "./confirm-dialog.tsx";
import { HashtagDialog } from "./hashtag-dialog.tsx";
import { Fullscreen } from "./fullscreen.tsx";
import { DetailView } from "./detail-view.tsx";
import { CollectionFormModal } from "./collections-view.tsx";

type MenuAction = "favorite" | "hashtag" | "fullscreen" | "remove" | "delete";

interface MenuState {
  x: number;
  y: number;
  clip: Clip;
}

export function CollectionDetail({ collection, allHashtags, onBack, onChanged }: {
  collection: CollectionSummary;
  allHashtags: string[];
  onBack: () => void;
  onChanged: () => void;
}) {
  const [clips, setClips] = useState<Clip[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [menu, setMenu] = useState<MenuState | null>(null);
  const [tagDialog, setTagDialog] = useState<Clip | null>(null);
  const [confirmDel, setConfirmDel] = useState<Clip | null>(null);
  const [confirmRemove, setConfirmRemove] = useState<Clip | null>(null);
  const [fullscreen, setFullscreen] = useState<{ clip: Clip; initialTime: number } | null>(null);
  const [detailIndex, setDetailIndex] = useState<number | null>(null);
  const [renaming, setRenaming] = useState(false);
  const [confirmDeleteCol, setConfirmDeleteCol] = useState(false);
  const [busy, setBusy] = useState(false);
  const menuRef = useRef<HTMLDivElement | null>(null);

  const refetch = useCallback(async (silent = false) => {
    if (!silent) setLoading(true);
    try {
      setClips(await collectionsApi.clips(collection.id));
      setError(null);
    } catch (err) {
      setError(String(err));
    } finally {
      setLoading(false);
    }
  }, [collection.id]);

  useEffect(() => {
    refetch();
  }, [refetch]);

  const updateClip = (next: Clip) => {
    setClips((items) => items.map((item) => (
      item.source === next.source && item.id === next.id ? next : item
    )));
    setTagDialog((item) => item && item.source === next.source && item.id === next.id ? next : item);
    setFullscreen((item) => (
      item && item.clip.source === next.source && item.clip.id === next.id
        ? { ...item, clip: next }
        : item
    ));
  };

  // Local context menu: same outside-click/Escape close as ContextMenu.
  useEffect(() => {
    if (!menu) return;
    const onDown = (e: MouseEvent) => {
      if (menuRef.current && !menuRef.current.contains(e.target as Node)) setMenu(null);
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") setMenu(null);
    };
    document.addEventListener("mousedown", onDown, true);
    document.addEventListener("keydown", onKey, true);
    return () => {
      document.removeEventListener("mousedown", onDown, true);
      document.removeEventListener("keydown", onKey, true);
    };
  }, [menu]);

  const openMenu = (e: MouseEvent, clip: Clip) =>
    setMenu({ x: e.clientX, y: e.clientY, clip });

  const onMenuAction = async (action: MenuAction, clip: Clip) => {
    switch (action) {
      case "favorite": {
        setMenu(null);
        const next = { ...clip, favorite: !clip.favorite };
        updateClip(next);
        const res = await clipApi.setFavorite(clip, next.favorite);
        if (!res.ok) updateClip(clip);
        break;
      }
      case "hashtag":
        setMenu(null);
        setTagDialog(clip);
        break;
      case "fullscreen":
        setMenu(null);
        setFullscreen({ clip, initialTime: 0 });
        break;
      case "remove":
        setMenu(null);
        setConfirmRemove(clip);
        break;
      case "delete":
        setMenu(null);
        setConfirmDel(clip);
        break;
    }
  };

  const doRemove = async () => {
    if (!confirmRemove || busy) return;
    setBusy(true);
    const res = await collectionsApi.removeClip(collection.id, confirmRemove);
    setBusy(false);
    if (res.ok) {
      setConfirmRemove(null);
      setDetailIndex(null);
      await refetch();
      onChanged();
    }
  };

  const doDelete = async () => {
    if (!confirmDel || busy) return;
    setBusy(true);
    const res = await clipApi.delete(confirmDel);
    setBusy(false);
    if (res.ok) {
      setConfirmDel(null);
      setDetailIndex(null);
      await refetch();
      onChanged();
    }
  };

  const deleteCollection = async () => {
    const res = await collectionsApi.remove(collection.id);
    if (res.ok) onBack();
    else setError(res.error ?? "Delete failed");
  };

  return (
    <>
      <div class="collection-detail-header">
        <button class="btn-ghost" title="Back" onClick={onBack}>
          <Icon name="chevron-left" size={16} /> Back
        </button>
        <span class="color-swatch" style={{ background: collection.color }} />
        <h2 class="collection-name">{collection.name}</h2>
        <span class="grid-count">
          <b>{clips.length}</b> {clips.length === 1 ? "clip" : "clips"}
        </span>
        <span class="detail-name-actions" style={{ marginLeft: "auto" }}>
          <button class="act-btn" title="Rename collection" onClick={() => setRenaming(true)}>
            <Icon name="pencil" size={16} />
          </button>
          <button class="act-btn" title="Delete collection" onClick={() => setConfirmDeleteCol(true)}>
            <Icon name="trash-2" size={16} />
          </button>
        </span>
      </div>

      {error && <p class="err">{error}</p>}

      {loading
        ? (
          <div class="empty">
            <span class="loading-dots"><i /><i /><i /></span>
            <div class="empty-hint">Loading collection…</div>
          </div>
        )
        : clips.length === 0
        ? (
          <div class="empty">
            <div class="empty-glyph"><Icon name="album" size={26} /></div>
            <div class="empty-title">No clips in this collection yet</div>
            <div class="empty-hint">Right-click a clip in the library → Add to collection.</div>
          </div>
        )
        : (
          <div class="grid">
            {clips.map((c, i) => (
              <ClipCard
                key={`${c.source}:${c.id}`}
                clip={c}
                onChanged={updateClip}
                onContextMenu={openMenu}
                onFullscreen={(c, t) => setFullscreen({ clip: c, initialTime: t })}
                onOpenDetail={() => setDetailIndex(i)}
              />
            ))}
          </div>
        )}

      {menu && (
        <div
          class="ctx-menu"
          ref={menuRef}
          style={{
            left: `${Math.min(menu.x, globalThis.innerWidth - 210)}px`,
            top: `${Math.min(menu.y, globalThis.innerHeight - 200)}px`,
          }}
          role="menu"
        >
          <button
            class={`ctx-item ctx-favorite ${menu.clip.favorite ? "active" : ""}`}
            onClick={() => onMenuAction("favorite", menu.clip)}
          >
            <span class="ctx-ico"><Icon name="star" size={15} filled={menu.clip.favorite} /></span>
            {menu.clip.favorite ? "Remove favorite" : "Add to favorites"}
          </button>
          <button class="ctx-item" onClick={() => onMenuAction("hashtag", menu.clip)}>
            <span class="ctx-ico"><Icon name="hash" size={15} /></span>Hashtags...
          </button>
          <button class="ctx-item" onClick={() => onMenuAction("fullscreen", menu.clip)}>
            <span class="ctx-ico"><Icon name="maximize" size={15} /></span>Fullscreen
          </button>
          <button class="ctx-item" onClick={() => onMenuAction("remove", menu.clip)}>
            <span class="ctx-ico"><Icon name="album" size={15} /></span>Remove from collection
          </button>
          <div class="ctx-sep" />
          <button class="ctx-item ctx-danger" onClick={() => onMenuAction("delete", menu.clip)}>
            <span class="ctx-ico"><Icon name="trash-2" size={15} /></span>Delete
          </button>
        </div>
      )}

      {tagDialog && (
        <HashtagDialog
          clip={tagDialog}
          allHashtags={allHashtags}
          onClose={() => setTagDialog(null)}
          onChanged={() => {
            refetch(true);
            onChanged();
          }}
        />
      )}

      {fullscreen && (
        <Fullscreen
          clip={fullscreen.clip}
          initialTime={fullscreen.initialTime}
          onClose={() => setFullscreen(null)}
        />
      )}

      {detailIndex !== null && clips.length > 0 && (
        <DetailView
          clips={clips}
          index={Math.min(detailIndex, clips.length - 1)}
          allHashtags={allHashtags}
          onIndex={setDetailIndex}
          onClose={() => setDetailIndex(null)}
          onChanged={refetch}
          onClipUpdate={updateClip}
          onDelete={setConfirmDel}
        />
      )}

      {renaming && (
        <CollectionFormModal
          key={collection.id}
          title="Rename collection"
          submitLabel="Save"
          initialName={collection.name}
          initialColor={collection.color}
          onSave={async (name) => {
            const res = await collectionsApi.rename(collection.id, name);
            if (!res.ok) throw new Error(res.error ?? "Rename failed");
            onChanged();
            return true;
          }}
          onCancel={() => setRenaming(false)}
        />
      )}

      {confirmDeleteCol && (
        <ConfirmDialog
          title="Delete collection"
          message={`Delete "${collection.name}"? This removes the collection but not the clips.`}
          confirmLabel="Delete"
          danger
          onConfirm={deleteCollection}
          onCancel={() => setConfirmDeleteCol(false)}
        />
      )}

      {confirmRemove && (
        <ConfirmDialog
          title="Remove from collection"
          message={`Remove "${confirmRemove.title || confirmRemove.video_file}" from this collection? The clip stays in your library.`}
          confirmLabel="Remove"
          onConfirm={doRemove}
          onCancel={() => setConfirmRemove(null)}
        />
      )}

      {confirmDel && (
        <ConfirmDialog
          title="Delete clip"
          message={`Permanently delete "${confirmDel.title || confirmDel.video_file}"? This removes the video and its thumbnail from disk.`}
          confirmLabel="Delete"
          danger
          onConfirm={doDelete}
          onCancel={() => setConfirmDel(null)}
        />
      )}
    </>
  );
}