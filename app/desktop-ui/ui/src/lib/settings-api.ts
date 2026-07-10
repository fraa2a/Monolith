// Frontend client for the settings popup. Talks to the Rust host over native
// Tauri IPC (invoke), which reads/writes settings.db and triggers the engine
// reload.

import { invoke } from "@tauri-apps/api/core";

export type Config = Record<string, any>;

export async function getConfig(): Promise<Config | null> {
  try {
    const config = await invoke<Config | null>("get_settings");
    return config ?? null;
  } catch {
    return null;
  }
}

export async function saveConfig(config: Config): Promise<{ ok: boolean; error?: string }> {
  try {
    await invoke("save_settings", { config });
    return { ok: true };
  } catch (err) {
    return { ok: false, error: String(err) };
  }
}

// Runtime capabilities (available encoders, monitors, input devices) published
// by the engine to runtime-status.json. Read-only; used to capability-gate the
// UI (never offer an encoder/monitor the machine can't do). Best-effort.
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
    return await invoke<RuntimeStatus>("runtime_status");
  } catch {
    return {};
  }
}

// Opens the native Windows folder picker via the Rust host and returns the
// chosen absolute path, or null if the user cancelled. `current` seeds the
// dialog's starting directory when it points at an existing folder.
export async function pickFolder(current?: string): Promise<string | null> {
  try {
    return await invoke<string | null>("pick_folder", { current: current ?? "" });
  } catch {
    return null;
  }
}
