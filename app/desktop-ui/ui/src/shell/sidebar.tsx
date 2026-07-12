import type { Filter } from "../lib/api.ts";
import { Icon } from "./icons.tsx";

interface Props {
  filter: Filter;
  onChange: (f: Filter) => void;
  onOpenSettings: () => void;
}

// App navigation rail. Library/Favorites drive the same clip filter the toolbar
// uses; Settings opens the popup. Two views for now.
export function Sidebar({ filter, onChange, onOpenSettings }: Props) {
  const fav = !!filter.favorite;

  return (
    <aside class="sidebar">
      <nav class="side-nav">
        <button
          class={!fav ? "side-item active" : "side-item"}
          onClick={() => onChange({ ...filter, favorite: undefined })}
          title="Library"
          aria-label="Library"
        >
          <Icon name="layout-grid" />
        </button>
        <button
          class={fav ? "side-item active fav-active" : "side-item"}
          onClick={() => onChange({ ...filter, favorite: true })}
          title="Favorites"
          aria-label="Favorites"
        >
          <Icon name="star" filled={fav} />
        </button>
      </nav>

      <div class="side-spacer" />

      <button class="side-item" onClick={onOpenSettings} title="Settings" aria-label="Settings">
        <Icon name="settings" />
      </button>
    </aside>
  );
}
