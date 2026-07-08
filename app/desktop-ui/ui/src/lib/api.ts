// Frontend API client for the local UI server (same origin as the webview).

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
  discord_app_id?: string | null;
  game_icon_url?: string | null;
  game_cover_url?: string | null;
  favorite: boolean;
  hashtags: string[];
  size_bytes: number;
}

export interface Filter {
  game?: string;
  hashtag?: string;
  favorite?: boolean;
  search?: string;
}

export async function fetchClips(filter: Filter = {}): Promise<Clip[]> {
  const q = new URLSearchParams();
  if (filter.game) q.set("game", filter.game);
  if (filter.hashtag) q.set("hashtag", filter.hashtag);
  if (filter.favorite) q.set("favorite", "1");
  if (filter.search) q.set("search", filter.search);
  const res = await fetch(`/api/clips?${q.toString()}`);
  const data = await res.json();
  return data.clips ?? [];
}

export async function fetchGames(): Promise<string[]> {
  const res = await fetch("/api/games");
  return (await res.json()).games ?? [];
}

export async function fetchHashtags(): Promise<string[]> {
  const res = await fetch("/api/hashtags");
  return (await res.json()).hashtags ?? [];
}

type MutationMethod =
  | "clip_set_favorite"
  | "clip_add_hashtag"
  | "clip_remove_hashtag"
  | "clip_rename"
  | "clip_set_title"
  | "clip_regen_thumb"
  | "clip_delete";

async function mutate(
  method: MutationMethod,
  body: Record<string, unknown>,
): Promise<{ ok: boolean; error?: string }> {
  const res = await fetch("/api/mutate", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ method, ...body }),
  });
  return res.json();
}

async function postJson(path: string, body: Record<string, unknown>): Promise<{ ok: boolean; error?: string; [key: string]: unknown }> {
  try {
    const res = await fetch(path, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body),
    });
    return res.json();
  } catch (err) {
    return { ok: false, error: String(err) };
  }
}

export const clipApi = {
  setFavorite: (c: Clip, favorite: boolean) =>
    mutate("clip_set_favorite", { source: c.source, id: c.id, favorite }),
  addHashtag: (c: Clip, tag: string) =>
    mutate("clip_add_hashtag", { source: c.source, id: c.id, tag }),
  removeHashtag: (c: Clip, tag: string) =>
    mutate("clip_remove_hashtag", { source: c.source, id: c.id, tag }),
  // Renames the on-disk file (advanced action). new_name is a stem, no extension.
  rename: (c: Clip, new_name: string) =>
    mutate("clip_rename", { source: c.source, id: c.id, new_name }),
  // Edits the display title only; the file on disk is untouched.
  setTitle: (c: Clip, title: string) =>
    mutate("clip_set_title", { source: c.source, id: c.id, title }),
  // Asks the engine to rebuild a missing/corrupt thumbnail.
  regenThumb: (c: Clip) =>
    mutate("clip_regen_thumb", { source: c.source, id: c.id }),
  delete: (c: Clip) => mutate("clip_delete", { source: c.source, id: c.id }),
  setDuration: (c: Clip, duration: number) =>
    postJson("/api/clip-duration", { source: c.source, id: c.id, duration }),
  saveCapturedThumb: (c: Clip, dataUrl: string) =>
    postJson("/api/thumb-capture", { source: c.source, id: c.id, data_url: dataUrl }),
};

// Subscribes to live clip-list changes via Server-Sent Events. Calls `onChange`
// whenever the engine reports a new clip. Returns an unsubscribe function.
export function subscribeClips(onChange: () => void): () => void {
  let source: EventSource | null = null;
  let closed = false;
  const connect = () => {
    if (closed) return;
    source = new EventSource("/api/events");
    source.addEventListener("clips", () => onChange());
    source.onerror = () => {
      // EventSource auto-reconnects; nothing to do. Guard against tight loops
      // by letting the browser honour the server's `retry` hint.
    };
  };
  connect();
  return () => {
    closed = true;
    source?.close();
  };
}

export function mediaUrl(c: Clip): string {
  return `/media/${c.source}/${encodeURIComponent(c.video_file)}`;
}

export function thumbUrl(c: Clip): string | null {
  if (!c.thumbnail_file) return null;
  return `/thumb/${c.source}/${encodeURIComponent(c.thumbnail_file)}`;
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
    const res = await fetch("/api/game-catalog");
    return (await res.json()).catalog ?? {};
  } catch {
    return {};
  }
}

// Lazily resolves + caches a game's icon URL (Discord CDN/catalog cache).
export async function fetchGameIcon(processName: string): Promise<string | null> {
  try {
    const res = await fetch(`/api/game-icon?process=${encodeURIComponent(processName)}`);
    return (await res.json()).icon ?? null;
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
    const q = new URLSearchParams();
    if (clip.discord_app_id) q.set("app_id", clip.discord_app_id);
    if (clip.game_process_name) q.set("process", clip.game_process_name);
    const res = await fetch(`/api/game-artwork?${q.toString()}`);
    return await res.json();
  } catch {
    return { icon: null, cover: null };
  }
}

export interface EngineStatus {
  recording?: boolean;
  paused?: boolean;
  replay_enabled?: boolean;
  recording_enabled?: boolean;
  clip_generation?: number;
}

export async function fetchEngineStatus(): Promise<EngineStatus> {
  try {
    const res = await fetch("/api/engine-status");
    return (await res.json()).status ?? {};
  } catch {
    return {};
  }
}
