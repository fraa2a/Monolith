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
  saveCapturedThumb: (c: Clip, dataUrl: string) =>
    ok(invoke("thumb_capture", { source: c.source, id: c.id, dataUrl })),
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

// Native icon extracted from an executable, as a base64 data: URL (null when
// the file has no icon). Preferred over remote artwork for status backgrounds.
export async function exeIconUrl(executablePath: string): Promise<string | null> {
  try {
    return await invoke<string | null>("exe_icon", { path: executablePath });
  } catch {
    return null;
  }
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
