// Frontend client for the settings popup (talks to /api/settings on the local
// UI server, which reads/writes settings.db and triggers the engine reload).

// deno-lint-ignore no-explicit-any
export type Config = Record<string, any>;

export async function getConfig(): Promise<Config | null> {
  const res = await fetch("/api/settings");
  const data = await res.json();
  return data.config ?? null;
}

export async function saveConfig(config: Config): Promise<{ ok: boolean; error?: string }> {
  const res = await fetch("/api/settings", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ config }),
  });
  return res.json();
}

// Runtime capabilities (available encoders, monitors, input devices) published
// by the engine to runtime-status.json. Read-only; used to capability-gate the
// UI (never offer an encoder/monitor the machine can't do). Best-effort - the
// server exposes it at /api/runtime-status.
export interface RuntimeStatus {
  available_encoders?: string[];
  active_encoder?: string;
  active_monitor_device?: string;
  monitors?: { device: string; width: number; height: number; primary: boolean }[];
  input_devices?: { id: string; name: string; default_device: boolean; available?: boolean }[];
  audio_sessions?: {
    process_id: number;
    process_name: string;
    display_name: string;
    executable_path?: string;
    window_title?: string;
    window_class?: string;
  }[];
  active_game?: {
    process_id: number;
    process_name: string;
    display_name: string;
    executable_path?: string;
    confidence?: number;
    reason?: string;
    capture_mode?: string;
    process_loopback_available?: boolean;
    last_switch_time?: string;
    poll_interval_ms?: number;
    fast_scan_enabled?: boolean;
  };
}

export async function getRuntimeStatus(): Promise<RuntimeStatus> {
  try {
    const res = await fetch("/api/runtime-status");
    return (await res.json()).status ?? {};
  } catch {
    return {};
  }
}

// Opens the native Windows folder picker via the Rust host and returns the chosen
// absolute path, or null if the user cancelled. `current` seeds the dialog's
// starting directory when it points at an existing folder.
export async function pickFolder(current?: string): Promise<string | null> {
  try {
    const res = await fetch("/api/pick-folder", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ current: current ?? "" }),
    });
    const data = await res.json();
    return data.ok ? (data.path as string) : null;
  } catch {
    return null;
  }
}
