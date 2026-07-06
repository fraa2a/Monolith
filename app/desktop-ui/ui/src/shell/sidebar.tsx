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
      <div class="side-section">Library</div>
      <nav class="side-nav">
        <button
          class={!fav ? "side-item active" : "side-item"}
          onClick={() => onChange({ ...filter, favorite: undefined })}
        >
          <Icon name="layout-grid" /> <span>Library</span>
        </button>
        <button
          class={fav ? "side-item active" : "side-item"}
          onClick={() => onChange({ ...filter, favorite: true })}
        >
          <Icon name="star" filled={fav} /> <span>Favorites</span>
        </button>
      </nav>

      <div class="side-spacer" />

      <button class="side-item" onClick={onOpenSettings}>
        <Icon name="settings" /> <span>Settings</span>
      </button>
    </aside>
  );
}
