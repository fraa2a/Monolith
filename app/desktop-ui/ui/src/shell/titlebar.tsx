import { appWindow } from "../lib/window.ts";

interface Props {
  view: string;
}

// Custom title bar (the window is decorations-less). The whole bar is a drag
// region except the control buttons; double-click toggles maximize. The view
// name reflects the current page (Library / Favorites).
export function Titlebar({ view }: Props) {
  const onMouseDown = (e: MouseEvent) => {
    if (e.button !== 0) return;
    if ((e.target as HTMLElement).closest(".tb-btn")) return;
    appWindow.startDrag();
  };

  return (
    <div class="titlebar" onMouseDown={onMouseDown} onDblClick={() => appWindow.toggleMaximize()}>
      <div class="tb-drag">
        <span class="tb-app">MONOLITH</span>
        <span class="tb-sep">/</span>
        <span class="tb-view">{view}</span>
      </div>

      <div class="tb-controls">
        <button class="tb-btn" title="Minimize" onClick={() => appWindow.minimize()}>
          <svg width="11" height="11" viewBox="0 0 11 11" aria-hidden="true">
            <rect x="1" y="5.5" width="9" height="1" fill="currentColor" />
          </svg>
        </button>
        <button class="tb-btn" title="Maximize" onClick={() => appWindow.toggleMaximize()}>
          <svg width="11" height="11" viewBox="0 0 11 11" aria-hidden="true">
            <rect x="1.5" y="1.5" width="8" height="8" fill="none" stroke="currentColor" stroke-width="1" />
          </svg>
        </button>
        <button class="tb-btn tb-close" title="Close" onClick={() => appWindow.close()}>
          <svg width="11" height="11" viewBox="0 0 11 11" aria-hidden="true">
            <path d="M1.5 1.5l8 8M9.5 1.5l-8 8" stroke="currentColor" stroke-width="1.1" />
          </svg>
        </button>
      </div>
    </div>
  );
}
