import { useCallback, useEffect, useState } from "preact/hooks";
import {
  type Clip,
  clipApi,
  fetchClips,
  fetchGames,
  fetchHashtags,
  type Filter,
  subscribeClips,
} from "./lib/api.ts";
import { ClipCard } from "./home/clip-card.tsx";
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

  const reload = useCallback(async (silent = false) => {
    if (!silent) setLoading(true);
    const [c, g, h] = await Promise.all([
      fetchClips(filter),
      fetchGames(),
      fetchHashtags(),
    ]);
    setClips(c);
    setGames(g);
    setHashtags(h);
    setLoading(false);
  }, [filter]);

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

  const openMenu = (e: MouseEvent, clip: Clip) =>
    setMenu({ x: e.clientX, y: e.clientY, clip });

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
      <Titlebar view={filter.favorite ? "Favorites" : "Library"} />
      <div class="shell">
        <Sidebar
          filter={filter}
          onChange={setFilter}
          onOpenSettings={() => setShowSettings(true)}
        />
        <main class="content">
          <Filters
            games={games}
            hashtags={hashtags}
            filter={filter}
            onChange={setFilter}
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
              {clips.map((c, i) => (
                <ClipCard
                  key={`${c.source}:${c.id}`}
                  clip={c}
                  onChanged={(next) => {
                    updateClip(next);
                    reload(true);
                  }}
                  onContextMenu={openMenu}
                  onFullscreen={(c, t) => setFullscreen({ clip: c, initialTime: t })}
                  onOpenDetail={() => setDetailIndex(i)}
                />
              ))}
            </div>
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
        />
      )}

      {showSettings && <SettingsPopup onClose={() => setShowSettings(false)} />}
    </div>
  );
}
