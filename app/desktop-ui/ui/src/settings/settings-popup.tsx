import { useEffect, useRef, useState } from "preact/hooks";
import { type Config, getConfig, getRuntimeStatus, type RuntimeStatus, saveConfig } from "../lib/settings-api.ts";
import { AudioSettings } from "./audio-settings.tsx";
import { Icon } from "../shell/icons.tsx";

type Page =
  | "general"
  | "output"
  | "clip"
  | "capture"
  | "audio"
  | "hotkeys"
  | "advanced"
  | "game";

const PAGES: { id: Page; label: string; icon: string }[] = [
  { id: "general", label: "General", icon: "sliders-horizontal" },
  { id: "output", label: "Output", icon: "folder" },
  { id: "clip", label: "Clip", icon: "film" },
  { id: "capture", label: "Capture", icon: "monitor" },
  { id: "audio", label: "Audio", icon: "volume-2" },
  { id: "hotkeys", label: "Hotkeys", icon: "keyboard" },
  { id: "advanced", label: "Advanced", icon: "gauge" },
  { id: "game", label: "Game Detection", icon: "gamepad" },
];

type SaveState = "idle" | "saving" | "saved" | "error";

interface Props {
  onClose: () => void;
}

// deno-lint-ignore no-explicit-any
function getPath(obj: any, path: string): any {
  return path.split(".").reduce((o, k) => (o == null ? undefined : o[k]), obj);
}

// Returns a deep-ish clone with `path` set to `value` (mutates a fresh copy).
// deno-lint-ignore no-explicit-any
function setPath(obj: any, path: string, value: any): any {
  const copy = structuredClone(obj);
  const keys = path.split(".");
  // deno-lint-ignore no-explicit-any
  let cur: any = copy;
  for (let i = 0; i < keys.length - 1; i++) {
    if (cur[keys[i]] == null || typeof cur[keys[i]] !== "object") cur[keys[i]] = {};
    cur = cur[keys[i]];
  }
  cur[keys[keys.length - 1]] = value;
  return copy;
}

// The settings popup: a custom overlay with a page list on the left and the
// current page's controls on the right. Every edit is auto-saved (debounced):
// it writes the whole config to settings.db and triggers the engine reload.
// Capability-gated by runtime-status (encoders/monitors/devices).
export function SettingsPopup({ onClose }: Props) {
  const [draft, setDraft] = useState<Config | null>(null);
  const [rs, setRs] = useState<RuntimeStatus>({});
  const [page, setPage] = useState<Page>("general");
  const [saveState, setSaveState] = useState<SaveState>("idle");
  const [error, setError] = useState<string | null>(null);

  // Skip auto-save for the freshly-loaded config (only user edits should save).
  const skipSave = useRef(true);
  const saveTimer = useRef<number | undefined>(undefined);
  const savedTimer = useRef<number | undefined>(undefined);

  useEffect(() => {
    (async () => {
      const [cfg, status] = await Promise.all([getConfig(), getRuntimeStatus()]);
      setDraft(cfg);
      setRs(status);
    })();
  }, []);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") onClose();
    };
    document.addEventListener("keydown", onKey, true);
    return () => document.removeEventListener("keydown", onKey, true);
  }, [onClose]);

  // Debounced auto-save: fires ~500ms after the last edit.
  useEffect(() => {
    if (!draft || skipSave.current) return;
    setSaveState("saving");
    clearTimeout(saveTimer.current);
    saveTimer.current = setTimeout(async () => {
      const res = await saveConfig(draft);
      if (res.ok) {
        setSaveState("saved");
        setError(null);
        clearTimeout(savedTimer.current);
        savedTimer.current = setTimeout(() => setSaveState("idle"), 1600);
      } else {
        setSaveState("error");
        setError(res.error ?? "save failed");
      }
    }, 500);
    return () => clearTimeout(saveTimer.current);
  }, [draft]);

  // deno-lint-ignore no-explicit-any
  const update = (path: string, value: any) => {
    skipSave.current = false;
    setDraft((d) => (d ? setPath(d, path, value) : d));
  };

  return (
    <div class="modal-backdrop" onMouseDown={onClose}>
      <div class="settings" onMouseDown={(e) => e.stopPropagation()}>
        <div class="settings-nav">
          <div class="settings-title">Settings</div>
          {PAGES.map((pg) => (
            <button
              key={pg.id}
              class={pg.id === page ? "settings-tab active" : "settings-tab"}
              onClick={() => setPage(pg.id)}
            >
              <Icon name={pg.icon} size={16} />
              <span>{pg.label}</span>
            </button>
          ))}

          <div class="side-spacer" />
          {saveState !== "idle" && (
            <div class={`save-status ${saveState}`}>
              {saveState === "saving" && <span class="save-dot" />}
              {saveState === "saving"
                ? "Saving…"
                : saveState === "saved"
                ? "All changes saved"
                : (error ?? "Save failed")}
            </div>
          )}
        </div>

        <div class="settings-body">
          {!draft
            ? <div class="empty">Loading settings… (is the engine running?)</div>
            : (
              <Fields
                page={page}
                cfg={draft}
                rs={rs}
                update={update}
              />
            )}
        </div>
      </div>
    </div>
  );
}

// ── Field controls ────────────────────────────────────────────────────────────

interface FieldsProps {
  page: Page;
  cfg: Config;
  rs: RuntimeStatus;
  // deno-lint-ignore no-explicit-any
  update: (path: string, value: any) => void;
}

function Fields({ page, cfg, rs, update }: FieldsProps) {
  const bool = (path: string, label: string, help?: string) => (
    <label class="field field-row">
      <input
        type="checkbox"
        checked={!!getPath(cfg, path)}
        onChange={(e) => update(path, (e.target as HTMLInputElement).checked)}
      />
      <span>
        <span class="field-label">{label}</span>
        {help && <span class="field-help">{help}</span>}
      </span>
    </label>
  );

  const text = (path: string, label: string, help?: string) => (
    <div class="field">
      <label class="field-label">{label}</label>
      <input
        class="input"
        value={getPath(cfg, path) ?? ""}
        onInput={(e) => update(path, (e.target as HTMLInputElement).value)}
      />
      {help && <span class="field-help">{help}</span>}
    </div>
  );

  const num = (path: string, label: string, min?: number, max?: number, help?: string) => (
    <div class="field">
      <label class="field-label">{label}</label>
      <input
        class="input"
        type="number"
        min={min}
        max={max}
        value={getPath(cfg, path) ?? 0}
        onInput={(e) => update(path, Number((e.target as HTMLInputElement).value))}
      />
      {help && <span class="field-help">{help}</span>}
    </div>
  );

  const select = (path: string, label: string, opts: { value: string; label: string }[], help?: string) => (
    <div class="field">
      <label class="field-label">{label}</label>
      <select
        class="select"
        value={String(getPath(cfg, path) ?? "")}
        onChange={(e) => update(path, (e.target as HTMLSelectElement).value)}
      >
        {opts.map((o) => <option value={o.value} key={o.value}>{o.label}</option>)}
      </select>
      {help && <span class="field-help">{help}</span>}
    </div>
  );

  // Edits a string[] as newline-separated text.
  const list = (path: string, label: string, help?: string) => (
    <div class="field">
      <label class="field-label">{label}</label>
      <textarea
        class="input textarea"
        value={(getPath(cfg, path) ?? []).join("\n")}
        onInput={(e) =>
          update(
            path,
            (e.target as HTMLTextAreaElement).value.split("\n").map((s) => s.trim()).filter(Boolean),
          )}
      />
      {help && <span class="field-help">{help}</span>}
    </div>
  );

  switch (page) {
    case "general":
      return (
        <>
          {bool("update.auto_check", "Automatic update checks")}
          {bool("replay_buffer.enabled", "Replay buffer enabled", "Rolling clip buffer")}
          {bool("recording.enabled", "Manual recording enabled")}
        </>
      );

    case "output":
      return (
        <>
          {text("output.clips_directory", "Clips folder")}
          {text("output.recordings_directory", "Recordings folder")}
          {select("replay_buffer.save_container", "Clip container", [
            { value: "mkv", label: "MKV" },
            { value: "mp4", label: "MP4" },
          ])}
          {select("recording.container", "Recording container", [
            { value: "mkv", label: "MKV" },
            { value: "mp4", label: "MP4" },
          ])}
        </>
      );

    case "clip":
      return (
        <>
          {num("replay_buffer.duration_seconds", "Replay duration (s)", 5, 600)}
          {num("replay_buffer.memory_budget_mb", "Memory budget (MB)", 32, 4096)}
        </>
      );

    case "capture": {
      const monOpts = [{ value: "", label: "Primary monitor" }].concat(
        (rs.monitors ?? []).map((m) => ({
          value: m.device,
          label: `${m.device} (${m.width}×${m.height})${m.primary ? " • primary" : ""}`,
        })),
      );
      return (
        <>
          {select("capture.monitor_device", "Monitor", monOpts)}
          {select("capture.resolution_mode", "Resolution", [
            { value: "source", label: "Match source" },
            { value: "custom", label: "Custom" },
          ])}
          {getPath(cfg, "capture.resolution_mode") === "custom" && (
            <>
              {num("capture.resolution_width", "Width", 320, 7680)}
              {num("capture.resolution_height", "Height", 240, 4320)}
            </>
          )}
          {bool("capture.show_capture_border", "Show capture border")}
        </>
      );
    }

    case "audio":
      return <AudioSettings cfg={cfg} update={update} />;

    case "hotkeys":
      return (
        <>
          {text("hotkeys.save_replay", "Save replay")}
          {text("hotkeys.recording_start", "Start recording")}
          {text("hotkeys.recording_stop", "Stop recording")}
          {text("hotkeys.pause_resume", "Pause / resume")}
        </>
      );

    case "advanced": {
      const encOpts = [{ value: "auto", label: "Auto (probe best)" }].concat(
        (rs.available_encoders ?? []).map((e) => ({ value: e, label: e })),
      );
      return (
        <>
          {select("video_encoder.backend", "Encoder backend", encOpts,
            rs.active_encoder ? `Active: ${rs.active_encoder}` : undefined)}
          {num("video_encoder.fps", "FPS", 15, 120)}
          {num("video_encoder.quality", "Quality (CQP/CRF)", 10, 30)}
          {select("video_encoder.scaling_filter", "Scaling filter", [
            { value: "bilinear", label: "Bilinear" },
            { value: "bicubic", label: "Bicubic" },
            { value: "lanczos", label: "Lanczos" },
          ])}
          {text("video_encoder.extra_ffmpeg_options", "Extra FFmpeg options",
            "key=value:key=value")}
          {text("output.temp_directory", "Temp folder")}
          {bool("discord.rich_presence_enabled", "Discord Rich Presence",
            "Show Monolith + current game in Discord (engine support pending)")}
        </>
      );
    }

    case "game":
      return (
        <>
          {select("capture_mode.mode", "Capture mode", [
            { value: "always", label: "Always capture" },
            { value: "game_only", label: "Only while a game is running" },
          ], "game_only stops the replay buffer when idle; never stops a recording")}
          {getPath(cfg, "capture_mode.mode") === "game_only" &&
            num("capture_mode.idle_timeout_seconds", "Idle timeout (s)", 30, 3600)}
          {bool("active_game.detection_enabled", "Detect active game")}
          {num("active_game.poll_interval_ms", "Poll interval (ms)", 3000, 30000)}
          {num("active_game.switch_debounce_ms", "Switch debounce (ms)", 1000, 15000)}
          {num("active_game.min_confidence", "Min confidence", 0, 100)}
          {bool("active_game.fast_scan_enabled", "Fast scan on foreground change")}
          {num("active_game.fast_scan_min_interval_ms", "Fast scan min interval (ms)", 500, 5000)}
          {list("active_game.blacklist_processes", "Blacklist (one exe per line)")}
          {list("active_game.whitelist_processes", "Whitelist (one exe per line)")}
          {list("active_game.manual_games", "Manual games (one exe per line)")}
        </>
      );
  }
}
