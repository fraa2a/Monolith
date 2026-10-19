// Custom title-bar window controls, driven by the native Tauri window API
// (the webview now loads bundled assets and has the Tauri JS API available).

import { flushConfig, settingsError } from "./settings-api.ts";
import { useEffect } from "preact/hooks";
import { getCurrentWindow } from "@tauri-apps/api/window";

const win = getCurrentWindow();

export const appWindow = {
  isMaximized: () => win.isMaximized(),
  minimize: () => win.minimize(),
  maximize: () => win.maximize(),
  unmaximize: () => win.unmaximize(),
  toggleMaximize: () => win.toggleMaximize(),
  close: async () => {
    await flushConfig();
    if (!settingsError.value) await win.close();
  },
  // startDragging() hands off to the OS move-loop; one call on mousedown is enough.
  startDrag: () => win.startDragging(),
};

let playerTransition: Promise<void> = Promise.resolve();

export function usePlayerWindow(active: boolean): void {
  useEffect(() => {
    if (!active) return;
    let closed = false;
    let changed = false;
    playerTransition = playerTransition.then(async () => {
      const maximized = await appWindow.isMaximized();
      if (!closed && !maximized) {
        await appWindow.maximize();
        changed = true;
      }
    }).catch((error) => console.error("Player window transition failed", error));
    return () => {
      closed = true;
      playerTransition = playerTransition.then(async () => {
        if (changed) await appWindow.unmaximize();
      }).catch((error) => console.error("Player window restore failed", error));
    };
  }, [active]);
}

export function useSettingsClose(): void {
  useEffect(() => {
    let disposed = false;
    let closing = false;
    let unlisten: (() => void) | undefined;
    void win.onCloseRequested(async (event) => {
      event.preventDefault();
      if (closing || disposed) return;
      closing = true;
      try {
        await flushConfig();
        if (!settingsError.value && !disposed) await win.destroy();
      } catch (error) {
        settingsError.value = String(error);
      } finally {
        closing = false;
      }
    }).then((stop) => {
      if (disposed) stop();
      else unlisten = stop;
    }).catch((error) => { settingsError.value = String(error); });
    return () => { disposed = true; unlisten?.(); };
  }, []);
}
