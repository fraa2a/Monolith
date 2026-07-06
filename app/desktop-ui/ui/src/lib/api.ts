// Frontend API client for the local UI server (same origin as the webview).

export type ClipSource = "replay" | "manual";

export interface Clip {
  id: number;
  source: ClipSource;
  video_file: string;
  thumbnail_file: string | null;
  created_at_utc: string;
  duration_seconds: number | null;
  game_process_name: string | null;
  game_display_name: string | null;
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

export const clipApi = {
  setFavorite: (c: Clip, favorite: boolean) =>
    mutate("clip_set_favorite", { source: c.source, id: c.id, favorite }),
  addHashtag: (c: Clip, tag: string) =>
    mutate("clip_add_hashtag", { source: c.source, id: c.id, tag }),
  removeHashtag: (c: Clip, tag: string) =>
    mutate("clip_remove_hashtag", { source: c.source, id: c.id, tag }),
  rename: (c: Clip, new_name: string) =>
    mutate("clip_rename", { source: c.source, id: c.id, new_name }),
  delete: (c: Clip) => mutate("clip_delete", { source: c.source, id: c.id }),
};

export function mediaUrl(c: Clip): string {
  return `/media/${c.source}/${encodeURIComponent(c.video_file)}`;
}

export function thumbUrl(c: Clip): string | null {
  if (!c.thumbnail_file) return null;
  return `/thumb/${c.source}/${encodeURIComponent(c.thumbnail_file)}`;
}

export interface CatalogEntry {
  display_name: string;
  icon_url: string | null;
}

// process_name_lower → entry. Used to enrich game display/icons in the grid.
export async function fetchGameCatalog(): Promise<Record<string, CatalogEntry>> {
  try {
    const res = await fetch("/api/game-catalog");
    return (await res.json()).catalog ?? {};
  } catch {
    return {};
  }
}

// Lazily resolves + caches a game's icon URL (Discord CDN).
export async function fetchGameIcon(processName: string): Promise<string | null> {
  try {
    const res = await fetch(`/api/game-icon?process=${encodeURIComponent(processName)}`);
    return (await res.json()).icon ?? null;
  } catch {
    return null;
  }
}
