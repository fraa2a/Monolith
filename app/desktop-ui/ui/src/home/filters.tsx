import type { Filter } from "../lib/api.ts";

interface Props {
  games: string[];
  hashtags: string[];
  filter: Filter;
  onChange: (f: Filter) => void;
}

// Content toolbar: search + game / hashtag filters. App identity, Favorites and
// Settings now live in the sidebar rail (see shell/sidebar.tsx).
export function Filters({ games, hashtags, filter, onChange }: Props) {
  const set = (patch: Partial<Filter>) => onChange({ ...filter, ...patch });

  return (
    <div class="toolbar">
      <div class="search-wrap">
        <input
          class="input search"
          placeholder="Search clips…"
          value={filter.search ?? ""}
          onInput={(e) => set({ search: (e.target as HTMLInputElement).value || undefined })}
        />
      </div>

      <div class="spacer" />

      <select
        class="select"
        value={filter.game ?? ""}
        onChange={(e) => set({ game: (e.target as HTMLSelectElement).value || undefined })}
      >
        <option value="">All games</option>
        {games.map((g) => <option value={g} key={g}>{g}</option>)}
      </select>

      <select
        class="select"
        value={filter.hashtag ?? ""}
        onChange={(e) => set({ hashtag: (e.target as HTMLSelectElement).value || undefined })}
      >
        <option value="">All hashtags</option>
        {hashtags.map((t) => <option value={t} key={t}>#{t}</option>)}
      </select>
    </div>
  );
}
