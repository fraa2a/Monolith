// Custom title-bar window controls. The webview loads an external http:// origin
// so it has no Tauri JS API; instead these POST to the local server, which
// dispatches to the real window via the AppHandle (see src-tauri/server.rs).

async function post(path: string): Promise<Record<string, unknown>> {
  try {
    const res = await fetch(path, { method: "POST" });
    return await res.json().catch(() => ({}));
  } catch {
    return {};
  }
}

export const appWindow = {
  minimize: () => post("/api/window/minimize"),
  toggleMaximize: () => post("/api/window/toggle-maximize"),
  close: () => post("/api/window/close"),
  // start_dragging() on the Rust side hands off to the OS move-loop.
  startDrag: () => post("/api/window/drag"),
};
