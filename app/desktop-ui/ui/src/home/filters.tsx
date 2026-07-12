import type { Filter } from "../lib/api.ts";
import { FilterMenu } from "./filter-menu.tsx";

interface Props {
  games: string[];
  hashtags: string[];
  filter: Filter;
  onChange: (f: Filter) => void;
}

// Content toolbar: icon filter menus (game / hashtag) on the left, a small
// search box on the right. Each filter opens a custom searchable dropdown.
export function Filters({ games, hashtags, filter, onChange }: Props) {
  const set = (patch: Partial<Filter>) => onChange({ ...filter, ...patch });

  return (
    <div class="toolbar">
      <div class="filter-menus">
        <FilterMenu
          icon="gamepad"
          title="Filter by game"
          placeholder="Search games…"
          allLabel="All games"
          options={games}
          value={filter.game}
          onChange={(v) => set({ game: v })}
        />
        <FilterMenu
          icon="hash"
          title="Filter by hashtag"
          placeholder="Search hashtags…"
          allLabel="All hashtags"
          options={hashtags}
          value={filter.hashtag}
          onChange={(v) => set({ hashtag: v })}
          format={(t) => `#${t}`}
        />
      </div>

      <div class="spacer" />

      <div class="search-wrap">
        <input
          class="input search"
          placeholder="Search clips…"
          value={filter.search ?? ""}
          onInput={(e) => set({ search: (e.target as HTMLInputElement).value || undefined })}
        />
      </div>
    </div>
  );
}
