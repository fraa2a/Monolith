import { useEffect, useState } from "preact/hooks";
import { type Config, getRuntimeStatus } from "../lib/settings-api.ts";

interface Props {
  cfg: Config;
  // deno-lint-ignore no-explicit-any
  update: (path: string, value: any) => void;
}

interface Session {
  process_id: number;
  process_name: string;
  display_name: string;
}
interface Device {
  id: string;
  name: string;
  default_device: boolean;
}
interface AppPref {
  enabled: boolean;
  volume: number;
}

// Medal-style audio settings page (deno.md §7). The dynamic app list comes from
// the engine's live render sessions (runtime-status). Per-app enabled/volume and
// the specific-apps/all-pc toggle + separate-tracks toggle are edited here into
// the audio schema; static "Game Audio" and "Microphone" rows are always shown.
// NOTE: per-process volume/mixing enforcement is pending engine work in
// libs/audio — these preferences are persisted and surfaced now.
export function AudioSettings({ cfg, update }: Props) {
  const [sessions, setSessions] = useState<Session[]>([]);
  const [devices, setDevices] = useState<Device[]>([]);

  useEffect(() => {
    getRuntimeStatus().then((rs) => {
      // deno-lint-ignore no-explicit-any
      setSessions(((rs as any).audio_sessions ?? []) as Session[]);
      setDevices((rs.input_devices ?? []) as Device[]);
    });
  }, []);

  const audio = cfg.audio ?? {};
  const captureMode: string = audio.capture_mode ?? "all_pc_audio";
  const separateTracks: boolean = audio.separate_tracks ?? true;
  const prefs: Record<string, AppPref> = audio.app_preferences ?? {};

  const setPref = (proc: string, patch: Partial<AppPref>) => {
    const key = proc.toLowerCase();
    const cur = prefs[key] ?? { enabled: true, volume: 100 };
    update("audio.app_preferences", { ...prefs, [key]: { ...cur, ...patch } });
  };

  return (
    <>
      <div class="field">
        <label class="field-label">Audio capture</label>
        <select
          class="select"
          value={captureMode}
          onChange={(e) => update("audio.capture_mode", (e.target as HTMLSelectElement).value)}
        >
          <option value="all_pc_audio">All PC audio</option>
          <option value="specific_apps">Specific apps</option>
        </select>
      </div>

      <label class="field field-row">
        <input
          type="checkbox"
          checked={separateTracks}
          onChange={(e) => update("audio.separate_tracks", (e.target as HTMLInputElement).checked)}
        />
        <span>
          <span class="field-label">Separate audio tracks</span>
          <span class="field-help">
            On = each included source gets its own track. Microphones are always on
            their own track regardless.
          </span>
        </span>
      </label>

      {/* Static sources — always present (deno.md §7). */}
      <div class="audio-static">
        <div class="audio-row static">
          <span class="audio-ico">🎮</span>
          <span class="audio-name">Game Audio (active game)</span>
        </div>
        <div class="audio-row static">
          <span class="audio-ico">🎙</span>
          <span class="audio-name">Microphone</span>
        </div>
      </div>

      <div class="field">
        <label class="field-label">Primary microphone</label>
        <select
          class="select"
          value={String(audio.primary_microphone_device_id ?? "")}
          onChange={(e) =>
            update("audio.primary_microphone_device_id", (e.target as HTMLSelectElement).value)}
        >
          <option value="">Default input device</option>
          {devices.map((d) => (
            <option value={d.id} key={d.id}>
              {d.name}{d.default_device ? " • default" : ""}
            </option>
          ))}
        </select>
        <span class="field-help">Always routed to its own track.</span>
      </div>

      {captureMode === "specific_apps" && (
        <div class="field">
          <label class="field-label">Applications with active audio</label>
          {sessions.length === 0 && <span class="field-help">No active audio sessions right now.</span>}
          <div class="audio-list">
            {sessions.map((s) => {
              const key = s.process_name.toLowerCase();
              const pref = prefs[key] ?? { enabled: true, volume: 100 };
              return (
                <div class="audio-row" key={s.process_id + s.process_name}>
                  <input
                    type="checkbox"
                    checked={pref.enabled}
                    onChange={(e) => setPref(s.process_name, {
                      enabled: (e.target as HTMLInputElement).checked,
                    })}
                  />
                  <span class="audio-name" title={s.process_name}>
                    {s.display_name || s.process_name}
                  </span>
                  <input
                    class="vol-slider"
                    type="range"
                    min={0}
                    max={100}
                    value={pref.volume}
                    disabled={!pref.enabled}
                    onInput={(e) => setPref(s.process_name, {
                      volume: Number((e.target as HTMLInputElement).value),
                    })}
                  />
                  <span class="audio-vol">{pref.volume}%</span>
                </div>
              );
            })}
          </div>
        </div>
      )}
    </>
  );
}
