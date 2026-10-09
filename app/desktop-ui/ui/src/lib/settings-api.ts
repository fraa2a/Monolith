// Settings persist through the native host and trigger an engine reload.

import { signal } from "@preact/signals";
import { invoke } from "@tauri-apps/api/core";

export type Config = Record<string, any>;

export const settingsDraft = signal<Config | null>(null);
export const settingsSaveState = signal<"idle" | "saving" | "saved" | "error">("idle");
export const settingsError = signal<string | null>(null);
let loading: Promise<Config | null> | null = null;
let revision = 0;
let savedRevision = 0;
let timer: ReturnType<typeof setTimeout> | undefined;
let writing: Promise<void> | null = null;

export function getConfig(): Promise<Config | null> {
  if (settingsDraft.value) return Promise.resolve(settingsDraft.value);
  loading ??= invoke<Config | null>("get_settings").then((config) => {
    settingsDraft.value = config;
    return config;
  }).catch(() => null).finally(() => { loading = null; });
  return loading;
}

export function editConfig(mutate: (config: Config) => void, immediate = false): void {
  if (!settingsDraft.value) return;
  const draft = structuredClone(settingsDraft.value);
  mutate(draft);
  settingsDraft.value = draft;
  ++revision;
  settingsSaveState.value = "saving";
  settingsError.value = null;
  clearTimeout(timer);
  if (immediate) void flushConfig();
  else timer = setTimeout(() => { void flushConfig(); }, 500);
}

export function flushConfig(): Promise<void> {
  clearTimeout(timer);
  if (writing) return writing;
  writing = (async () => {
    while (savedRevision < revision && settingsDraft.value) {
      const current = revision;
      const config = structuredClone(settingsDraft.value);
      try {
        await invoke("save_settings", { config });
        savedRevision = current;
        if (current === revision) {
          settingsSaveState.value = "saved";
          settingsError.value = null;
        }
      } catch (error) {
        settingsSaveState.value = "error";
        settingsError.value = String(error);
        break;
      }
    }
  })().finally(() => { writing = null; });
  return writing;
}

// Engine capabilities are best-effort snapshots from runtime-status.json.
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
  // All DB-matched games running right now, for the multi-game picker.
  game_candidates?: {
    process_id: number;
    process_name: string;
    display_name: string;
    discord_app_id?: string;
    executable_path?: string;
    foreground?: boolean;
    fullscreen?: boolean;
  }[];
  // Which candidate the engine is currently recording/clipping (0 = auto).
  selected_game_pid?: number;
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
