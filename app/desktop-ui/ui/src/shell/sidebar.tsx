import type { Filter } from "../lib/api.ts";
import { Icon } from "./icons.tsx";

interface Props {
  filter: Filter;
  onChange: (f: Filter) => void;
  onOpenSettings: () => void;
  collectionsActive: boolean;
  onOpenCollections: () => void;
}

// App navigation rail. Library/Favorites drive the same clip filter the toolbar
// uses; Collections swaps the grid for the collections page; Settings opens the
// popup.
export function Sidebar({ filter, onChange, onOpenSettings, collectionsActive, onOpenCollections }: Props) {
  const fav = !!filter.favorite;

  return (
    <aside class="sidebar">
      <nav class="side-nav">
        <button
          class={!fav && !collectionsActive ? "side-item active" : "side-item"}
          onClick={() => onChange({ ...filter, favorite: undefined })}
          title="Library"
          aria-label="Library"
        >
          <Icon name="layout-grid" />
        </button>
        <button
          class={fav && !collectionsActive ? "side-item active fav-active" : "side-item"}
          onClick={() => onChange({ ...filter, favorite: true })}
          title="Favorites"
          aria-label="Favorites"
        >
          <Icon name="star" filled={fav} />
        </button>
        <button
          class={collectionsActive ? "side-item active" : "side-item"}
          onClick={onOpenCollections}
          title="Collections"
          aria-label="Collections"
        >
          <Icon name="album" />
        </button>
      </nav>

      <div class="side-spacer" />

      <button class="side-item" onClick={onOpenSettings} title="Settings" aria-label="Settings">
        <Icon name="settings" />
      </button>
    </aside>
  );
}
