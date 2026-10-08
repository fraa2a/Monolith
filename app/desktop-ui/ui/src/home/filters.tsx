import type { Filter } from "../lib/api.ts";
import { FilterMenu } from "./filter-menu.tsx";
import { Icon } from "../shell/icons.tsx";
import { Button } from "../components/ui/button.tsx";

interface Props {
  games: string[];
  hashtags: string[];
  filter: Filter;
  onChange: (f: Filter) => void;
  count: number;
}

// Content toolbar: icon filter menus (game / hashtag) on the left, a small
// search box on the right. Each filter opens a custom searchable dropdown.
export function Filters({ games, hashtags, filter, onChange, count }: Props) {
  const set = (patch: Partial<Filter>) => onChange({ ...filter, ...patch });

  return (
    <div class="toolbar">
      <h1 class="toolbar-heading">{filter.favorite ? "Favorites" : "Library"}</h1>
      <span class="toolbar-divider" aria-hidden="true" />
      <div class="filter-menus">
        <span class="toolbar-label">Filters</span>
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
      {(filter.game || filter.hashtag || filter.search) && <Button variant="ghost" size="sm" onClick={() => onChange({ favorite: filter.favorite })}>Clear filters</Button>}
      <span class="item-count">{count.toLocaleString()} {count === 1 ? "clip" : "clips"}</span>

      <div class="search-wrap">
        <Icon name="search" size={16} />
        <input
          class="input search"
          type="search"
          aria-label="Search clips"
          placeholder="Search clips…"
          value={filter.search ?? ""}
          onInput={(e) => set({ search: (e.target as HTMLInputElement).value || undefined })}
        />
      </div>
    </div>
  );
}
