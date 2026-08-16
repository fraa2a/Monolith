// Frontend API client. Talks to the Rust host over native Tauri IPC
// (invoke/listen) instead of the old loopback HTTP server.

import { convertFileSrc, invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

export type ClipSource = "replay" | "manual";

export interface Clip {
  id: number;
  source: ClipSource;
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

// Wraps a command invocation in the old { ok, error } envelope so call sites
// (built around fetch()'s always-resolves shape) don't need to change.
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
}

export const clipApi = {
  setFavorite: (c: Clip, favorite: boolean) =>
    ok(invoke("clip_set_favorite", { source: c.source, id: c.id, favorite })),
  addHashtag: (c: Clip, tag: string) =>
    ok(invoke("clip_add_hashtag", { source: c.source, id: c.id, tag })),
  removeHashtag: (c: Clip, tag: string) =>
    ok(invoke("clip_remove_hashtag", { source: c.source, id: c.id, tag })),
  // Renames the on-disk file (advanced action). new_name is a stem, no extension.
  rename: (c: Clip, new_name: string) =>
    ok(invoke("clip_rename", { source: c.source, id: c.id, newName: new_name })),
  // Edits the display title only; the file on disk is untouched.
  setTitle: (c: Clip, title: string) =>
    ok(invoke("clip_set_title", { source: c.source, id: c.id, title })),
  // Asks the engine to rebuild a missing/corrupt thumbnail.
  regenThumb: (c: Clip) => ok(invoke("clip_regen_thumb", { source: c.source, id: c.id })),
  delete: (c: Clip) => ok(invoke("clip_delete", { source: c.source, id: c.id })),
  setDuration: (c: Clip, duration: number) =>
    ok(invoke("clip_set_duration", { source: c.source, id: c.id, duration })),
  // thumb_capture returns the thumbnail filename; keep it on the envelope so
  // clip-card can update the grid row without a refetch.
  saveCapturedThumb: async (c: Clip, dataUrl: string): Promise<{
    ok: boolean;
    thumbnail_file?: string;
    error?: string;
  }> => {
    try {
      const thumbnail_file = await invoke<string>("thumb_capture", {
        source: c.source,
        id: c.id,
        dataUrl,
      });
      return { ok: true, thumbnail_file };
    } catch (err) {
      return { ok: false, error: String(err) };
    }
  },
  revealInExplorer: (c: Clip) => ok(invoke("reveal_in_explorer", { path: c.video_path })),
  trim: (c: Clip, start: number, end: number) =>
    ok(invoke("clip_trim", { source: c.source, id: c.id, start, end })),
  listBookmarks: (c: Clip) =>
    invoke<BookmarkRow[]>("clip_list_bookmarks", { source: c.source, id: c.id }),
  addBookmark: (c: Clip, timeSeconds: number, label: string, color: string) =>
    ok(invoke("clip_add_bookmark", { source: c.source, id: c.id, timeSeconds, label, color })),
  updateBookmark: (c: Clip, seq: number, label: string, color: string) =>
    ok(invoke("clip_update_bookmark", { source: c.source, id: c.id, seq, label, color })),
  deleteBookmark: (c: Clip, seq: number) =>
    ok(invoke("clip_delete_bookmark", { source: c.source, id: c.id, seq })),
  recordingAddBookmark: () => ok(invoke("recording_add_bookmark")),
};

export const collectionsApi = {
  list: () => invoke<CollectionSummary[]>("list_collections"),
  create: (name: string, color: string) => invoke<number>("create_collection", { name, color }),
  rename: (id: number, name: string) => ok(invoke("rename_collection", { id, name })),
  remove: (id: number) => ok(invoke("delete_collection", { id })),
  clips: (id: number) => invoke<Clip[]>("collection_clips", { collectionId: id }),
  addClip: (id: number, c: Clip) =>
    ok(invoke("add_clip_to_collection", { collectionId: id, source: c.source, clipId: c.id })),
  removeClip: (id: number, c: Clip) =>
    ok(invoke("remove_clip_from_collection", { collectionId: id, source: c.source, clipId: c.id })),
};

// Subscribes to live clip-list changes via a native Tauri event. Calls
// `onChange` whenever the engine reports a new clip. Returns an unsubscribe
// function.
export function subscribeClips(onChange: () => void): () => void {
  let unlisten: (() => void) | null = null;
  let cancelled = false;
  listen("clips", () => onChange()).then((fn) => {
    if (cancelled) {
      fn();
    } else {
      unlisten = fn;
    }
  });
  return () => {
    cancelled = true;
    unlisten?.();
  };
}

export function mediaUrl(c: Clip): string {
  return convertFileSrc(c.video_path);
}

export function thumbUrl(c: Clip): string | null {
  if (!c.thumbnail_path) return null;
  return convertFileSrc(c.thumbnail_path);
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

// Native icon extracted from an executable, as a base64 data: URL (null when
// the file has no icon). Preferred over remote artwork for status backgrounds.
// `processName` is the cache key on the Rust side (survives reinstalls/path
// changes); pass "" when unknown (skips caching for that one lookup).
const exeIconRequests = new Map<string, Promise<string | null>>();

export function exeIconUrl(executablePath: string, processName: string): Promise<string | null> {
  // Every card of the same game used to issue its own IPC round-trip (with a
  // base64 PNG payload each); share one promise per process/path instead.
  const key = `${processName}\u0000${executablePath}`;
  let request = exeIconRequests.get(key);
  if (!request) {
    request = invoke<string | null>("exe_icon", { path: executablePath, process: processName })
      .catch(() => null);
    exeIconRequests.set(key, request);
  }
  return request;
}

export interface EngineStatus {
  recording?: boolean;
  paused?: boolean;
  replay_enabled?: boolean;
  recording_enabled?: boolean;
  clip_generation?: number;
  connected?: boolean;
}

export async function fetchEngineStatus(): Promise<EngineStatus> {
  try {
    return await invoke<EngineStatus>("engine_status");
  } catch {
    return { connected: false };
  }
}
