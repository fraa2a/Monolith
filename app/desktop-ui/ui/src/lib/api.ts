
import { convertFileSrc, invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

export type ClipSource = "replay" | "manual";

export interface Clip {
  id: number;
  source: ClipSource;
  catalog_uid: string;
  clip_uid: string;
  media_revision: number;
  video_file: string;
  // User-facing display name, independent of the on-disk filename. Defaults to
  // "Untitled" for new clips; renaming edits this, not the file.
  title: string;
  thumbnail_file: string | null;
  created_at_utc: string;
  duration_seconds: number | null;
  game_process_name: string | null;
  game_display_name: string | null;
  game_executable_path?: string | null;
  discord_app_id?: string | null;
  game_icon_url?: string | null;
  game_cover_url?: string | null;
  favorite: boolean;
  hashtags: string[];
  size_bytes: number;
  // Absolute filesystem paths, used with convertFileSrc() for <video>/<img> src.
  video_path: string;
  thumbnail_path: string | null;
}

export function clipKey(clip: Clip): string {
  return JSON.stringify([clip.source, clip.catalog_uid || clip.video_path, clip.clip_uid || `${clip.id}:${clip.created_at_utc}`]);
}

export function clipIdentity(clip: Clip) {
  return { source: clip.source, id: clip.id, catalogUid: clip.catalog_uid, clipUid: clip.clip_uid };
}

export interface Filter {
  game?: string;
  hashtag?: string;
  favorite?: boolean;
  search?: string;
}

export async function fetchClips(filter: Filter = {}): Promise<Clip[]> {
  return invoke<Clip[]>("list_clips", {
    filter: {
      game: filter.game ?? null,
      hashtag: filter.hashtag ?? null,
      favorite: filter.favorite ?? null,
      search: filter.search ?? null,
    },
  });
}

export async function fetchGames(): Promise<string[]> {
  return invoke<string[]>("distinct_games");
}

export async function fetchHashtags(): Promise<string[]> {
  return invoke<string[]>("distinct_hashtags");
}

// Normalize command failures into the shared ok/error envelope.
async function ok(promise: Promise<unknown>): Promise<{ ok: boolean; error?: string }> {
  try {
    await promise;
    return { ok: true };
  } catch (err) {
    return { ok: false, error: String(err) };
  }
}

export interface BookmarkRow {
  seq: number;
  time_seconds: number;
  label: string;
  color: string;
}

export interface CollectionSummary {
  id: number;
  name: string;
  color: string;
  created_at_utc: string;
  clip_count: number;
  unresolved_count?: number;
}

export const clipApi = {
  snapshot: (c: Clip) => invoke<Clip>("clip_snapshot", { source: c.source, id: c.id }),
  setFavorite: (c: Clip, favorite: boolean) =>
    ok(invoke("clip_set_favorite", { ...clipIdentity(c), favorite })),
  addHashtag: (c: Clip, tag: string) =>
    ok(invoke("clip_add_hashtag", { ...clipIdentity(c), tag })),
  removeHashtag: (c: Clip, tag: string) =>
    ok(invoke("clip_remove_hashtag", { ...clipIdentity(c), tag })),
  // Renames the on-disk file (advanced action). new_name is a stem, no extension.
  rename: (c: Clip, new_name: string) =>
    ok(invoke("clip_rename", { ...clipIdentity(c), newName: new_name })),
  // Edits the display title only; the file on disk is untouched.
  setTitle: (c: Clip, title: string) =>
    ok(invoke("clip_set_title", { ...clipIdentity(c), title })),
  // Asks the engine to rebuild a missing/corrupt thumbnail.
  regenThumb: (c: Clip) => ok(invoke("clip_regen_thumb", { ...clipIdentity(c) })),
  delete: (c: Clip) => ok(invoke("clip_delete", { ...clipIdentity(c) })),
  setDuration: (c: Clip, duration: number) =>
    ok(invoke("clip_set_duration", { ...clipIdentity(c), mediaRevision: c.media_revision, duration })),
  // The card reads clip_snapshot after capture to receive the absolute path.
  saveCapturedThumb: async (c: Clip, dataUrl: string): Promise<{
    ok: boolean;
    thumbnail_file?: string;
    error?: string;
  }> => {
    try {
      const thumbnail_file = await invoke<string>("thumb_capture", {
        ...clipIdentity(c),
        mediaRevision: c.media_revision,
        dataUrl,
      });
      return { ok: true, thumbnail_file };
    } catch (err) {
      return { ok: false, error: String(err) };
    }
  },
  revealInExplorer: (c: Clip) => ok(invoke("reveal_in_explorer", { path: c.video_path })),
  trim: (c: Clip, start: number, end: number) =>
    ok(invoke("clip_trim", { ...clipIdentity(c), start, end })),
  listBookmarks: (c: Clip) =>
    invoke<BookmarkRow[]>("clip_list_bookmarks", { ...clipIdentity(c) }),
  addBookmark: (c: Clip, timeSeconds: number, label: string, color: string) =>
    ok(invoke("clip_add_bookmark", { ...clipIdentity(c), timeSeconds, label, color })),
  updateBookmark: (c: Clip, seq: number, label: string, color: string) =>
    ok(invoke("clip_update_bookmark", { ...clipIdentity(c), seq, label, color })),
  deleteBookmark: (c: Clip, seq: number) =>
    ok(invoke("clip_delete_bookmark", { ...clipIdentity(c), seq })),
  recordingAddBookmark: () => ok(invoke("recording_add_bookmark")),
};

export const collectionsApi = {
  list: () => invoke<CollectionSummary[]>("list_collections"),
  create: (name: string, color: string) => invoke<number>("create_collection", { name, color }),
  rename: (id: number, name: string) => ok(invoke("rename_collection", { id, name })),
  remove: (id: number) => ok(invoke("delete_collection", { id })),
  memberships: (c: Clip) => invoke<number[]>("collection_memberships", { source: c.source, id: c.id }),
  clips: (id: number) => invoke<Clip[]>("collection_clips", { collectionId: id }),
  addClip: (id: number, c: Clip) =>
    ok(invoke("add_clip_to_collection", { collectionId: id, source: c.source, clipId: c.id, catalogUid: c.catalog_uid, clipUid: c.clip_uid })),
  removeClip: (id: number, c: Clip) =>
    ok(invoke("remove_clip_from_collection", { collectionId: id, source: c.source, clipId: c.id, catalogUid: c.catalog_uid, clipUid: c.clip_uid })),
};

export function subscribeClips(onChange: () => void): () => void {
  let unlisten: (() => void) | null = null;
  let cancelled = false;
  listen("clips", () => onChange()).then((fn) => {
    if (cancelled) {
      fn();
    } else {
      unlisten = fn;
    }
  }).catch((err) => console.error("Clip subscription failed", err));
  return () => {
    cancelled = true;
    unlisten?.();
  };
}

export function mediaUrl(c: Clip): string {
  return convertFileSrc(c.video_path, "media");
}

export function thumbUrl(c: Clip): string | null {
  if (!c.thumbnail_path) return null;
  return convertFileSrc(c.thumbnail_path, "media");
}

export interface CatalogEntry {
  display_name: string;
  discord_app_id?: string | null;
  icon_url: string | null;
  cover_url?: string | null;
}

// process_name_lower -> entry. Used to enrich game display/icons in the grid.
export async function fetchGameCatalog(): Promise<Record<string, CatalogEntry>> {
  try {
    return await invoke<Record<string, CatalogEntry>>("game_catalog_map");
  } catch {
    return {};
  }
}

// Lazily resolves + caches a game's icon URL (Discord CDN/catalog cache).
export async function fetchGameIcon(processName: string): Promise<string | null> {
  try {
    return await invoke<string | null>("game_icon", { process: processName });
  } catch {
    return null;
  }
}

export interface GameArtwork {
  icon: string | null;
  cover: string | null;
  display_name?: string | null;
  discord_app_id?: string | null;
}

export async function fetchGameArtwork(clip: Pick<Clip, "discord_app_id" | "game_process_name">): Promise<GameArtwork> {
  try {
    return await invoke<GameArtwork>("game_artwork", {
      appId: clip.discord_app_id ?? null,
      process: clip.game_process_name ?? null,
    });
  } catch {
    return { icon: null, cover: null };
  }
}

// Recorder control, forwarded by the host to the engine over JSON-RPC.
export type RecorderCommand = "recording_start" | "recording_stop" | "save_replay";

export function recorderCommand(method: RecorderCommand): Promise<{ ok: boolean; error?: string }> {
  return ok(invoke("recorder_command", { method }));
}

// Picks which detected game the engine records/clips when several are running.
// Pass the executable basename, or "" / "auto" to return to automatic selection.
export function setSelectedGame(exe: string): Promise<{ ok: boolean; error?: string }> {
  return ok(invoke("set_selected_game", { exe, pid: null }));
}

// Use processName as the persistent icon cache key; an empty key disables caching.
const exeIconRequests = new Map<string, Promise<string | null>>();

export function exeIconUrl(executablePath: string, processName: string): Promise<string | null> {
  // Share one icon request per process/path across cards.
  const key = `${processName}\u0000${executablePath}`;
  let request = exeIconRequests.get(key);
  if (!request) {
    request = invoke<string | null>("exe_icon", { path: executablePath, process: processName })
      .catch(() => null)
      .then((icon) => {
        if (icon === null) exeIconRequests.delete(key);
        return icon;
      });
    if (exeIconRequests.size >= 128) {
      const oldest = exeIconRequests.keys().next().value;
      if (oldest !== undefined) exeIconRequests.delete(oldest);
    }
    exeIconRequests.set(key, request);
  }
  return request;
}

export interface EngineStatus {
  recording?: boolean;
  paused?: boolean;
  replay_enabled?: boolean;
  capture_running?: boolean;
  replay_running?: boolean;
  recording_error?: string;
  recording_enabled?: boolean;
  clip_generation?: number;
  // Engine component version (the interface version is separate - the two
  // are versioned independently by the component updater).
  version?: string;
  connected?: boolean;
}

export async function fetchEngineStatus(): Promise<EngineStatus> {
  try {
    return await invoke<EngineStatus>("engine_status");
  } catch {
    return { connected: false };
  }
}

// Opens the component updater window (Updater.exe). Fails when the updater
// exe is missing (dev trees without a CMake build).
export function openUpdater(): Promise<void> {
  return invoke("open_updater");
}
