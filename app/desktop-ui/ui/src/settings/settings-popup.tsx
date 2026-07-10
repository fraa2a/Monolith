import { useEffect, useRef, useState } from "preact/hooks";
import { type Config, getConfig, getRuntimeStatus, type RuntimeStatus, saveConfig } from "../lib/settings-api.ts";
import { AudioSettings } from "./audio-settings.tsx";
import {
  Field,
  FolderPicker,
  HotkeyCapture,
  Section,
  Segmented,
  Select,
  TextInput,
  Toggle,
} from "./controls.tsx";
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

// Bitrate presets (Mbps). The heaviest two are flagged so the UI can warn about
// disk usage.
const BITRATE_PRESETS = [3, 5, 7, 10, 15, 20, 25, 30, 35, 40, 50, 70, 100];
const FPS_PRESETS = [24, 30, 60, 120, 144];
const REPLAY_PRESETS = [15, 30, 60, 120];

type SaveState = "idle" | "saving" | "saved" | "error";

interface Props {
  onClose: () => void;
}

function getPath(obj: any, path: string): any {
  return path.split(".").reduce((o, k) => (o == null ? undefined : o[k]), obj);
}

function setPath(obj: any, path: string, value: any): any {
  const copy = structuredClone(obj);
  const keys = path.split(".");
  let cur: any = copy;
  for (let i = 0; i < keys.length - 1; i++) {
    if (cur[keys[i]] == null || typeof cur[keys[i]] !== "object") cur[keys[i]] = {};
    cur = cur[keys[i]];
  }
  cur[keys[keys.length - 1]] = value;
  return copy;
}

function findHotkeyConflicts(entries: { label: string; value: string }[]): Set<string> {
  const byNormalized = new Map<string, string[]>();
  for (const { label, value } of entries) {
    const normalized = value.trim().toLowerCase();
    if (!normalized || normalized === "none") continue;
    const labels = byNormalized.get(normalized) ?? [];
    labels.push(label);
    byNormalized.set(normalized, labels);
  }
  const conflicts = new Set<string>();
  for (const labels of byNormalized.values()) {
    if (labels.length > 1) labels.forEach((l) => conflicts.add(l));
  }
  return conflicts;
}

export function SettingsPopup({ onClose }: Props) {
  const [draft, setDraft] = useState<Config | null>(null);
  const [rs, setRs] = useState<RuntimeStatus>({});
  const [page, setPage] = useState<Page>("general");
  const [saveState, setSaveState] = useState<SaveState>("idle");
  const [error, setError] = useState<string | null>(null);

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
        savedTimer.current = setTimeout(() => setSaveState("idle"), 1600) as unknown as number;
      } else {
        setSaveState("error");
        setError(res.error ?? "save failed");
      }
    }, 500) as unknown as number;
    return () => clearTimeout(saveTimer.current);
  }, [draft]);

  const update = (path: string, value: any) => {
    skipSave.current = false;
    setDraft((d) => (d ? setPath(d, path, value) : d));
  };

  return (
    <div class="modal-backdrop" onMouseDown={onClose}>
      <div class="settings" onMouseDown={(e) => e.stopPropagation()}>
        <button
          class="settings-close"
          onMouseDown={(e) => e.stopPropagation()}
          onClick={onClose}
          title="Close (Esc)"
        >
          <Icon name="x" size={18} />
        </button>
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
            : <Pages page={page} cfg={draft} rs={rs} update={update} />}
        </div>
      </div>
    </div>
  );
}

// Replay duration: a preset dropdown (15/30/60/120) plus Custom, which enables a
// number field. Custom mode is tracked locally so it stays selected even when the
// custom value happens to equal a preset number.
function ReplayDurationSection(
  { value, onChange }: { value: number; onChange: (v: number) => void },
) {
  const [custom, setCustom] = useState(!REPLAY_PRESETS.includes(value));
  const mode = custom ? "custom" : String(value);
  return (
    <Section
      title="Replay Duration"
      description="How much gameplay the replay buffer keeps for instant clips."
    >
      <Field
        label="Duration"
        help="Choose a preset, or Custom to enter your own length."
        control={
          <Select
            value={mode}
            options={[
              ...REPLAY_PRESETS.map((s) => ({ value: String(s), label: `${s} seconds` })),
              { value: "custom", label: "Custom" },
            ]}
            onChange={(v) => {
              if (v === "custom") {
                setCustom(true);
              } else {
                setCustom(false);
                onChange(Number(v));
              }
            }}
          />
        }
      />
      <Field
        label="Custom length (seconds)"
        help="Enabled only when Custom is selected (5–600)."
        control={
          <TextInput
            type="number"
            min={5}
            max={600}
            value={value}
            disabled={!custom}
            onInput={(v) => onChange(Number(v))}
          />
        }
      />
    </Section>
  );
}

interface PagesProps {
  page: Page;
  cfg: Config;
  rs: RuntimeStatus;
  update: (path: string, value: any) => void;
}

function Pages({ page, cfg, rs, update }: PagesProps) {
  const val = (p: string) => getPath(cfg, p);

  switch (page) {
    case "general":
      return (
        <Section title="General" description="App-wide behaviour.">
          <Field
            label="Automatic update checks"
            help="Check for new Monolith releases in the background."
            control={
              <Toggle checked={!!val("update.auto_check")} onChange={(v) => update("update.auto_check", v)} />
            }
          />
          <Field
            label="Replay buffer"
            help="Keep a rolling buffer so you can save the last moments."
            control={
              <Toggle
                checked={!!val("replay_buffer.enabled")}
                onChange={(v) => update("replay_buffer.enabled", v)}
              />
            }
          />
          <Field
            label="Manual recording"
            help="Allow starting and stopping full recordings."
            control={
              <Toggle
                checked={!!val("recording.enabled")}
                onChange={(v) => update("recording.enabled", v)}
              />
            }
          />
        </Section>
      );

    case "output":
      return (
        <>
          <Section title="Folders" description="Where clips and recordings are saved.">
            <Field
              label="Clips folder"
              control={
                <FolderPicker
                  value={val("output.clips_directory") ?? ""}
                  onPick={(p) => update("output.clips_directory", p)}
                />
              }
            />
            <Field
              label="Recordings folder"
              control={
                <FolderPicker
                  value={val("output.recordings_directory") ?? ""}
                  onPick={(p) => update("output.recordings_directory", p)}
                />
              }
            />
          </Section>
          <Section title="Containers" description="File format for saved media.">
            <Field
              label="Clip container"
              control={
                <Select
                  value={String(val("replay_buffer.save_container") ?? "mkv")}
                  options={[{ value: "mkv", label: "MKV" }, { value: "mp4", label: "MP4" }]}
                  onChange={(v) => update("replay_buffer.save_container", v)}
                />
              }
            />
            <Field
              label="Recording container"
              control={
                <Select
                  value={String(val("recording.container") ?? "mkv")}
                  options={[{ value: "mkv", label: "MKV" }, { value: "mp4", label: "MP4" }]}
                  onChange={(v) => update("recording.container", v)}
                />
              }
            />
          </Section>
        </>
      );

    case "clip":
      return (
        <ReplayDurationSection
          value={Number(val("replay_buffer.duration_seconds") ?? 30)}
          onChange={(v) => update("replay_buffer.duration_seconds", v)}
        />
      );

    case "capture": {
      const monOpts = [{ value: "", label: "Primary monitor" }].concat(
        (rs.monitors ?? []).map((m) => ({
          value: m.device,
          label: `${m.device} (${m.width}×${m.height})${m.primary}`,
        })),
      );
      // Hide presets larger than the selected monitor to prevent upscaling.
      const selDev = String(val("capture.monitor_device") ?? "");
      const mon = (rs.monitors ?? []).find((m) => m.device === selDev) ??
        (rs.monitors ?? []).find((m) => m.primary);
      const nativeH = mon?.height ?? 0;
      const resOpts = [{ value: "source", label: "Match source (native)" }].concat(
        [480, 720, 1080, 1440]
          .filter((h) => nativeH === 0 || h < nativeH)
          .map((h) => ({ value: `${h}p`, label: `${h}p` })),
      );
      return (
        <Section title="Capture" description="Which display to record and at what resolution.">
          <Field
            label="Monitor"
            control={
              <Select
                value={selDev}
                options={monOpts}
                onChange={(v) => update("capture.monitor_device", v)}
              />
            }
          />
          <Field
            label="Resolution"
            help="Aspect ratio follows the monitor; options above native are hidden."
            control={
              <Select
                value={String(val("capture.resolution_preset") ?? "source")}
                options={resOpts}
                onChange={(v) => update("capture.resolution_preset", v)}
              />
            }
          />
          <Field
            label="Show capture border"
            help="Draw the yellow border around the captured display."
            control={
              <Toggle
                checked={!!val("capture.show_capture_border")}
                onChange={(v) => update("capture.show_capture_border", v)}
              />
            }
          />
        </Section>
      );
    }

    case "audio":
      return <AudioSettings cfg={cfg} rs={rs} update={update} />;

    case "hotkeys": {
      const hotkeyFields: { key: string; label: string }[] = [
        { key: "hotkeys.save_replay", label: "Save replay" },
        { key: "hotkeys.recording_start", label: "Start recording" },
        { key: "hotkeys.recording_stop", label: "Stop recording" },
        { key: "hotkeys.pause_resume", label: "Pause / resume" },
      ];
      const conflicts = findHotkeyConflicts(hotkeyFields.map((f) => ({
        label: f.label,
        value: String(val(f.key) ?? ""),
      })));
      return (
        <Section title="Hotkeys" description="Click a field and press the key combination.">
          {hotkeyFields.map((f) => (
            <Field
              key={f.key}
              label={f.label}
              help={conflicts.has(f.label) ? "Already assigned to another action." : undefined}
              control={
                <HotkeyCapture
                  value={String(val(f.key) ?? "")}
                  onChange={(v) => update(f.key, v)}
                  invalid={conflicts.has(f.label)}
                />
              }
            />
          ))}
        </Section>
      );
    }

    case "advanced": {
      const device = String(val("video_encoder.device") ?? "gpu");
      const codec = String(val("video_encoder.codec") ?? "h264");
      const bitrate = Number(val("video_encoder.bitrate_kbps") ?? 20000);
      // Capability-gate: only offer GPU when the machine actually has a hardware
      // encoder (nvenc/amf/qsv). Otherwise GPU would silently fall back to CPU.
      const hasHwEncoder = (rs.available_encoders ?? []).some((e) =>
        /nvenc|amf|qsv/.test(e)
      );
      const deviceOpts = [{ value: "cpu", label: "CPU", icon: "cpu" }];
      if (hasHwEncoder) deviceOpts.unshift({ value: "gpu", label: "GPU", icon: "monitor" });
      // Friendly codec names; the technical encoder name is a secondary detail.
      const codecOpts = [
        { value: "h264", label: "H.264 (AVC)" },
        { value: "h265", label: "H.265 (HEVC)" },
      ];
      const bitrateOpts = BITRATE_PRESETS.map((mbps) => ({
        value: String(mbps * 1000),
        label: `${mbps} Mbps${mbps >= 70 ? "  •  high disk usage" : ""}`,
        danger: mbps >= 70,
      }));
      return (
        <>
          <Section title="Encoder" description="Pick where encoding runs, then the codec.">
            <Field
              label="Encoder device"
              help={rs.active_encoder ? `Active: ${rs.active_encoder}` : "GPU is faster; CPU is more compatible."}
              control={
                <Segmented
                  value={hasHwEncoder ? device : "cpu"}
                  options={deviceOpts}
                  onChange={(v) => update("video_encoder.device", v)}
                />
              }
            />
            <Field
              label="Codec"
              control={
                <Select value={codec} options={codecOpts} onChange={(v) => update("video_encoder.codec", v)} />
              }
            />
          </Section>

          <Section title="Quality" description="Constant bitrate (CBR). Higher bitrate = better quality and larger files.">
            <Field
              label="Bitrate"
              control={
                <Select
                  value={String(bitrate)}
                  options={bitrateOpts}
                  onChange={(v) => update("video_encoder.bitrate_kbps", Number(v))}
                />
              }
            />
            <Field
              label="Frame rate"
              control={
                <Select
                  value={String(val("video_encoder.fps") ?? 60)}
                  options={FPS_PRESETS.map((f) => ({ value: String(f), label: `${f} FPS` }))}
                  onChange={(v) => update("video_encoder.fps", Number(v))}
                />
              }
            />
            <Field
              label="Scaling filter"
              help="Used only when the output resolution differs from native."
              control={
                <Select
                  value={String(val("video_encoder.scaling_filter") ?? "bilinear")}
                  options={[
                    { value: "bilinear", label: "Bilinear" },
                    { value: "bicubic", label: "Bicubic" },
                    { value: "lanczos", label: "Lanczos" },
                  ]}
                  onChange={(v) => update("video_encoder.scaling_filter", v)}
                />
              }
            />
          </Section>

          <Section title="Extras">
            <Field
              label="Extra FFmpeg options"
              help="Advanced: key=value:key=value passed to the encoder."
              control={
                <TextInput
                  value={String(val("video_encoder.extra_ffmpeg_options") ?? "")}
                  onInput={(v) => update("video_encoder.extra_ffmpeg_options", v)}
                  placeholder="key=value:key=value"
                />
              }
            />
            <Field
              label="Discord Rich Presence"
              help="Show Monolith + current game in Discord (engine support pending)."
              control={
                <Toggle
                  checked={!!val("discord.rich_presence_enabled")}
                  onChange={(v) => update("discord.rich_presence_enabled", v)}
                />
              }
            />
            <Field
              label="Logging"
              help="Writes monolith.log to the app data folder. Off by default."
              control={
                <Toggle
                  checked={!!val("advanced.logging_enabled")}
                  onChange={(v) => update("advanced.logging_enabled", v)}
                />
              }
            />
          </Section>
        </>
      );
    }

    case "game":
      return (
        <Section
          title="Game Detection"
          description="Detection is automatic (Discord + running processes) and runs every 5 seconds."
        >
          <Field
            label="Capture mode"
            help="Only-while-gaming stops the buffer when idle; never stops a recording."
            control={
              <Select
                value={String(val("capture_mode.mode") ?? "always")}
                options={[
                  { value: "always", label: "Always capture" },
                  { value: "game_only", label: "Only while a game is running" },
                ]}
                onChange={(v) => update("capture_mode.mode", v)}
              />
            }
          />
          {val("capture_mode.mode") === "game_only" && (
            <Field
              label="Idle timeout (seconds)"
              control={
                <TextInput
                  type="number"
                  min={30}
                  max={3600}
                  value={Number(val("capture_mode.idle_timeout_seconds") ?? 300)}
                  onInput={(v) => update("capture_mode.idle_timeout_seconds", Number(v))}
                />
              }
            />
          )}
          <Field
            label="Detect active game"
            help="Automatically switch game-audio capture to the foreground game."
            control={
              <Toggle
                checked={!!val("active_game.detection_enabled")}
                onChange={(v) => update("active_game.detection_enabled", v)}
              />
            }
          />
        </Section>
      );
  }
}
