import { useCallback, useEffect, useState } from "preact/hooks";
import {
  collectionsApi,
  type Clip,
  clipApi,
  type CollectionSummary,
  fetchClips,
  fetchGames,
  fetchHashtags,
  type Filter,
  subscribeClips,
} from "./lib/api.ts";
import { ClipCard } from "./home/clip-card.tsx";
import { CollectionDetail } from "./home/collection-detail.tsx";
import { CollectionPicker } from "./home/collection-picker.tsx";
import { CollectionsView } from "./home/collections-view.tsx";
import { ContextMenu, type MenuAction } from "./home/context-menu.tsx";
import { ConfirmDialog } from "./home/confirm-dialog.tsx";
import { HashtagDialog } from "./home/hashtag-dialog.tsx";
import { Fullscreen } from "./home/fullscreen.tsx";
import { DetailView } from "./home/detail-view.tsx";
import { Filters } from "./home/filters.tsx";
import { SettingsPopup } from "./settings/settings-popup.tsx";
import { Titlebar } from "./shell/titlebar.tsx";
import { Sidebar } from "./shell/sidebar.tsx";

interface MenuState {
  x: number;
  y: number;
  clip: Clip;
}

// Clicking the already-active nav entry must not refire the reload effect:
// keep the previous filter object when the values are identical.
const sameFilter = (a: Filter, b: Filter) =>
  a.game === b.game && a.hashtag === b.hashtag && a.favorite === b.favorite && a.search === b.search;

export function App() {
  const [clips, setClips] = useState<Clip[]>([]);
  const [games, setGames] = useState<string[]>([]);
  const [hashtags, setHashtags] = useState<string[]>([]);
  const [filter, setFilter] = useState<Filter>({});
  const [loading, setLoading] = useState(true);

  const [menu, setMenu] = useState<MenuState | null>(null);
  const [tagDialog, setTagDialog] = useState<Clip | null>(null);
  const [confirmDel, setConfirmDel] = useState<Clip | null>(null);
  const [fullscreen, setFullscreen] = useState<{ clip: Clip; initialTime: number } | null>(null);
  const [detailIndex, setDetailIndex] = useState<number | null>(null);
  const [showSettings, setShowSettings] = useState(false);
  const [collectionView, setCollectionView] = useState<{ kind: "list" } | { kind: "detail"; id: number } | null>(null);
  const [collections, setCollections] = useState<CollectionSummary[]>([]);
  const [collectionPicker, setCollectionPicker] = useState<Clip | null>(null);

  const reload = useCallback(async (silent = false) => {
    if (!silent) setLoading(true);
    // In collection view the grid isn't shown: skip the clips+games scans
    // (each is a full catalog pass); hashtags stay for the detail filter UI.
    const [c, g, h, cols] = await Promise.all([
      collectionView ? Promise.resolve([] as Clip[]) : fetchClips(filter),
      collectionView ? Promise.resolve([] as string[]) : fetchGames(),
      fetchHashtags(),
      collectionView ? collectionsApi.list() : Promise.resolve(null as CollectionSummary[] | null),
    ]);
    setClips(c);
    setGames(g);
    setHashtags(h);
    if (cols) setCollections(cols);
    setLoading(false);
  }, [filter, collectionView]);

  const updateClip = useCallback((next: Clip) => {
    setClips((items) => items.map((item) => (
      item.source === next.source && item.id === next.id ? next : item
    )));
    setTagDialog((item) => item && item.source === next.source && item.id === next.id ? next : item);
    setFullscreen((item) => (
      item && item.clip.source === next.source && item.clip.id === next.id
        ? { ...item, clip: next }
        : item
    ));
  }, []);

  useEffect(() => {
    reload();
  }, [reload]);

  useEffect(() => {
    const unsubscribe = subscribeClips(() => reload(true));
    return unsubscribe;
  }, [reload]);

  useEffect(() => {
    const block = (e: MouseEvent) => e.preventDefault();
    document.addEventListener("contextmenu", block);
    return () => document.removeEventListener("contextmenu", block);
  }, []);

  // Stable callbacks: ClipCard is memoized, so inline closures here would
  // re-render the whole grid on every unrelated state change.
  const openMenu = useCallback((e: MouseEvent, clip: Clip) => {
    setMenu({ x: e.clientX, y: e.clientY, clip });
  }, []);

  const openFullscreen = useCallback((clip: Clip, initialTime: number) => {
    setFullscreen({ clip, initialTime });
  }, []);

  const openDetail = useCallback((clip: Clip) => {
    const i = clips.findIndex((item) => item.source === clip.source && item.id === clip.id);
    if (i >= 0) setDetailIndex(i);
  }, [clips]);

  // Card-level mutations (favorite, thumbnail capture, duration fix) are all
  // patched optimistically via updateClip — a full catalog reload per event
  // caused a reload storm when many cards were missing thumbnails.
  const handleCardChanged = useCallback((next: Clip) => {
    updateClip(next);
  }, [updateClip]);

  const changeFilter = useCallback((next: Filter) => {
    setFilter((prev) => (sameFilter(prev, next) ? prev : next));
  }, []);

  const onMenuAction = async (action: MenuAction, clip: Clip) => {
    switch (action) {
      case "favorite": {
        const next = { ...clip, favorite: !clip.favorite };
        updateClip(next);
        const res = await clipApi.setFavorite(clip, next.favorite);
        if (!res.ok) updateClip(clip);
        else await reload(true);
        break;
      }
      case "hashtag":
        setMenu(null);
        setTagDialog(clip);
        break;
      case "add-to-collection":
        setMenu(null);
        setCollectionPicker(clip);
        break;
      case "fullscreen":
        setFullscreen({ clip, initialTime: 0 });
        break;
      case "delete":
        setMenu(null);
        setConfirmDel(clip);
        break;
    }
  };

  const doDelete = async () => {
    if (!confirmDel) return;
    await clipApi.delete(confirmDel);
    setConfirmDel(null);
    setDetailIndex(null);
    await reload(true);
  };

  return (
    <div class="win">
      <Titlebar
        view={
          collectionView?.kind === "detail"
            ? `Collections · ${collections.find((c) => c.id === collectionView.id)?.name ?? "Collections"}`
            : collectionView
            ? "Collections"
            : filter.favorite
            ? "Favorites"
            : "Library"
        }
      />
      <div class="shell">
        <Sidebar
          filter={filter}
          onChange={changeFilter}
          onOpenSettings={() => setShowSettings(true)}
          collectionsActive={collectionView != null}
          onOpenCollections={() => setCollectionView({ kind: "list" })}
        />
        <main class="content">
          {collectionView ? (
            collectionView.kind === "list" ? (
              <CollectionsView
                collections={collections}
                onOpen={(id) => setCollectionView({ kind: "detail", id })}
                onChanged={() => reload(true)}
              />
            ) : (
              (() => {
                const c = collections.find((item) => item.id === collectionView.id);
                return c
                  ? (
                    <CollectionDetail
                      collection={c}
                      allHashtags={hashtags}
                      onBack={() => setCollectionView({ kind: "list" })}
                      onChanged={() => reload(true)}
                    />
                  )
                  : (
                    <CollectionsView
                      collections={collections}
                      onOpen={(id) => setCollectionView({ kind: "detail", id })}
                      onChanged={() => reload(true)}
                    />
                  );
              })()
            )
          ) : (
            <>
          <Filters
            games={games}
            hashtags={hashtags}
            filter={filter}
            onChange={changeFilter}
          />

          {loading
        ? (
          <div class="empty">
            <span class="loading-dots"><i /><i /><i /></span>
            <div class="empty-hint">Loading your library…</div>
          </div>
        )
        : clips.length === 0
        ? (
          <div class="empty">
            <div class="empty-glyph">
              <svg viewBox="0 0 24 24" width="26" height="26" aria-hidden="true">
                <path
                  fill="currentColor"
                  d="M4 5h16a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1zm1 2v8l4-4 3 3 3-3 4 4V7H5z"
                />
              </svg>
            </div>
            <div class="empty-title">No clips yet</div>
            <div class="empty-hint">Press Ctrl+Shift+F8 while capturing to save your first clip.</div>
          </div>
        )
        : (
          <>
            <div class="grid">
              {clips.map((c) => (
                <ClipCard
                  key={`${c.source}:${c.id}`}
                  clip={c}
                  onChanged={handleCardChanged}
                  onContextMenu={openMenu}
                  onFullscreen={openFullscreen}
                  onOpenDetail={openDetail}
                />
              ))}
            </div>
            </>
          )}
            </>
          )}
        </main>
      </div>

      {menu && (
        <ContextMenu
          x={menu.x}
          y={menu.y}
          clip={menu.clip}
          onAction={onMenuAction}
          onClose={() => setMenu(null)}
        />
      )}

      {tagDialog && (
        <HashtagDialog
          clip={tagDialog}
          allHashtags={hashtags}
          onClose={() => setTagDialog(null)}
          onChanged={() => reload(true)}
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

      {collectionPicker && (
        <CollectionPicker
          clip={collectionPicker}
          onClose={() => setCollectionPicker(null)}
          onChanged={() => reload(true)}
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
          allHashtags={hashtags}
          onIndex={setDetailIndex}
          onClose={() => setDetailIndex(null)}
          onChanged={() => reload(true)}
          onClipUpdate={updateClip}
          onDelete={setConfirmDel}
          onAddToCollection={(clip) => setCollectionPicker(clip)}
        />
      )}

      {showSettings && <SettingsPopup onClose={() => setShowSettings(false)} />}
    </div>
  );
}
