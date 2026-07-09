// Custom title-bar window controls, driven by the native Tauri window API
// (the webview now loads bundled assets and has the Tauri JS API available).

import { getCurrentWindow } from "@tauri-apps/api/window";

const win = getCurrentWindow();

export const appWindow = {
  minimize: () => win.minimize(),
  toggleMaximize: () => win.toggleMaximize(),
  close: () => win.close(),
  // startDragging() hands off to the OS move-loop; one call on mousedown is enough.
  startDrag: () => win.startDragging(),
};
