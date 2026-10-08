import type { Filter } from "../lib/api.ts";
import { Icon } from "./icons.tsx";

interface Props {
  filter: Filter;
  onChange: (f: Filter) => void;
  onOpenSettings: () => void;
  settingsActive: boolean;
  collectionsActive: boolean;
  onOpenCollections: () => void;
}

export function Sidebar({ filter, onChange, onOpenSettings, settingsActive, collectionsActive, onOpenCollections }: Props) {
  const fav = !!filter.favorite;

  return (
    <aside class="sidebar">
      <nav class="side-nav" aria-label="Main navigation">
        <button
          class={!fav && !collectionsActive && !settingsActive ? "side-item active" : "side-item"}
          aria-current={!fav && !collectionsActive && !settingsActive ? "page" : undefined}
          onClick={() => onChange({ ...filter, favorite: undefined })}
          title="Library"
          aria-label="Library"
        >
          <Icon name="layout-grid" />
        </button>
        <button
          class={fav && !collectionsActive && !settingsActive ? "side-item active" : "side-item"}
          aria-current={fav && !collectionsActive && !settingsActive ? "page" : undefined}
          onClick={() => onChange({ ...filter, favorite: true })}
          title="Favorites"
          aria-label="Favorites"
        >
          <Icon name="star" filled={fav} />
        </button>
        <button
          class={collectionsActive && !settingsActive ? "side-item active" : "side-item"}
          aria-current={collectionsActive && !settingsActive ? "page" : undefined}
          onClick={onOpenCollections}
          title="Collections"
          aria-label="Collections"
        >
          <Icon name="album" />
        </button>
      </nav>

      <div class="side-spacer" />

      <button class={settingsActive ? "side-item active" : "side-item"} onClick={onOpenSettings} title="Settings" aria-label="Settings" aria-current={settingsActive ? "page" : undefined}>
        <Icon name="settings" />
      </button>
    </aside>
  );
}
