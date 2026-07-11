import { type Config, type RuntimeStatus } from "../lib/settings-api.ts";
import { appLabel } from "../lib/format.ts";
import { Field, Section, Segmented, Select, Toggle, VolumeSlider } from "./controls.tsx";

interface Props {
  cfg: Config;
  rs: RuntimeStatus;
  update: (path: string, value: any) => void;
}

// One entry in audio.sources[] as the engine reads it.
interface Source {
  id: string;
  type: "desktop" | "input" | "process" | "active_game";
  name: string;
  device_id?: string;
  process_id?: number;
  process_name?: string;
  enabled: boolean;
  volume: number; // 0..1
  tracks: number[];
}

// Audio capture is modelled as a flat list of sources, each with its own
// enable + volume that the recorder actually applies to the mix. The UI groups
// them into Game Audio (desktop), Microphone (input), and any other apps that
// currently have audio. Editing anything switches audio.mode to "custom" so the
// engine honours the explicit source list.
export function AudioSettings({ cfg, rs, update }: Props) {
  const audio = cfg.audio ?? {};
  const sources: Source[] = Array.isArray(audio.sources) ? audio.sources : [];
  const devices = rs.input_devices ?? [];
  const sessions = rs.audio_sessions ?? [];

  const find = (pred: (s: Source) => boolean): Source | undefined => sources.find(pred);
  const game = find((s) => s.type === "desktop");
  const mic = find((s) => s.type === "input");

  // Custom mode uses the explicit source list, so it must always contain the two
  // baseline sources (Game Audio → track 1, Microphone → track 2) that the
  // default mode implies — otherwise editing one would silently drop the other.
  const withBaseline = (list: Source[]): Source[] => {
    const out = list.slice();
    if (!out.some((s) => s.type === "desktop")) {
      out.push({ id: "desktop", type: "desktop", name: "Game Audio", enabled: true, volume: 1, tracks: [1] });
    }
    if (!out.some((s) => s.type === "input")) {
      out.push({
        id: "mic",
        type: "input",
        name: "Microphone",
        device_id: String(audio.primary_microphone_device_id ?? ""),
        enabled: true,
        volume: 1,
        tracks: [2],
      });
    }
    return out;
  };

  // Rewrites the whole source list and forces custom mode so the engine applies it.
  const commit = (next: Source[]) => {
    update("audio.mode", "custom");
    update("audio.sources", withBaseline(next));
  };

  const upsert = (match: (s: Source) => boolean, make: () => Source, patch: Partial<Source>) => {
    const base = withBaseline(sources);
    const idx = base.findIndex(match);
    if (idx >= 0) base[idx] = { ...base[idx], ...patch };
    else base.push({ ...make(), ...patch });
    commit(base);
  };

  // Game Audio = all desktop audio → track 1.
  const gameEnabled = game?.enabled ?? true;
  const gameVolume = game?.volume ?? 1;
  const setGame = (patch: Partial<Source>) =>
    upsert(
      (s) => s.type === "desktop",
      () => ({ id: "desktop", type: "desktop", name: "Game Audio", enabled: true, volume: 1, tracks: [1] }),
      patch,
    );

  // Microphone = the chosen input device → track 2.
  const micEnabled = mic?.enabled ?? true;
  const micVolume = mic?.volume ?? 1;
  const micDevice = mic?.device_id ?? String(audio.primary_microphone_device_id ?? "");
  const setMic = (patch: Partial<Source>) =>
    upsert(
      (s) => s.type === "input",
      () => ({ id: "mic", type: "input", name: "Microphone", enabled: true, volume: 1, tracks: [2] }),
      patch,
    );

  // Other app sources currently producing audio (process loopback).
  const processFor = (procName: string): Source | undefined =>
    find((s) => s.type === "process" && s.process_name?.toLowerCase() === procName.toLowerCase());

  // Track layout: the microphone always gets its own track (track 2) — that's
  // never up for debate since a mixed-in mic can't be un-mixed later. Every
  // *other* audio source (game desktop audio + each other app) either all
  // share the game's track (single track) or each gets its own free track
  // (separate tracks, tracks 1/3/4/5/6). Defaults to "separate" to match the
  // behaviour this UI shipped with before the toggle existed.
  const trackLayout: "single" | "separate" = audio.track_layout === "single" ? "single" : "separate";

  const nextFreeTrack = (): number => {
    if (trackLayout === "single") return 1;
    const used = new Set(sources.flatMap((s) => (s.type === "input" ? [] : s.tracks ?? [])));
    for (let t = 3; t <= 6; t++) if (!used.has(t)) return t;
    return 1; // overflow: mix into game-audio track
  };

  const setTrackLayout = (mode: "single" | "separate") => {
    update("audio.track_layout", mode);
    const base = withBaseline(sources);
    if (mode === "single") {
      commit(base.map((s) => (s.type === "input" ? s : { ...s, tracks: [1] })));
      return;
    }
    // Switching to "separate": keep game audio on track 1, hand out fresh
    // free tracks (3..6) to every other non-mic source in list order.
    let next = 3;
    commit(base.map((s) => {
      if (s.type === "input") return s;
      if (s.type === "desktop") return { ...s, tracks: [1] };
      return { ...s, tracks: [Math.min(next++, 6)] };
    }));
  };

  const setProcess = (s: { process_id: number; process_name: string; display_name: string }, patch: Partial<Source>) =>
    upsert(
      (x) => x.type === "process" && x.process_name?.toLowerCase() === s.process_name.toLowerCase(),
      () => ({
        id: `process:${s.process_name.toLowerCase()}`,
        type: "process",
        name: s.display_name || s.process_name,
        process_id: s.process_id,
        process_name: s.process_name,
        enabled: true,
        volume: 1,
        tracks: [nextFreeTrack()],
      }),
      patch,
    );

  return (
    <>
      <Section
        title="Track Layout"
        description="The microphone always records on its own track. Choose how every other audio source (game audio, other apps) is recorded."
      >
        <Field
          label="Track layout"
          help={trackLayout === "single"
            ? "Game audio and every other app are mixed into one track."
            : "Game audio and each other app get their own track."}
          control={
            <Segmented
              value={trackLayout}
              options={[
                { value: "single", label: "One track" },
                { value: "separate", label: "Separate tracks" },
              ]}
              onChange={(v) => setTrackLayout(v as "single" | "separate")}
            />
          }
        />
      </Section>

      <Section title="Game Audio" description="Desktop audio, including the game you are playing.">
        <Field
          label="Capture game audio"
          control={<Toggle checked={gameEnabled} onChange={(v) => setGame({ enabled: v })} />}
        />
        <Field
          label="Volume"
          help="Applied to the recorded audio, not just the preview."
          control={
            <VolumeSlider
              value={gameVolume}
              disabled={!gameEnabled}
              onChange={(v) => setGame({ volume: v })}
            />
          }
        />
      </Section>

      <Section title="Microphone" description="Your input device, recorded on its own track.">
        <Field
          label="Capture microphone"
          control={<Toggle checked={micEnabled} onChange={(v) => setMic({ enabled: v })} />}
        />
        <Field
          label="Device"
          control={
            <Select
              value={micDevice}
              options={[{ value: "", label: "Default input device" }].concat(
                devices.map((d) => ({
                  value: d.id,
                  label: `${d.name}${d.default_device ? " • default" : ""}`,
                })),
              )}
              onChange={(v) => {
                update("audio.primary_microphone_device_id", v);
                setMic({ device_id: v });
              }}
            />
          }
        />
        <Field
          label="Volume"
          control={
            <VolumeSlider
              value={micVolume}
              disabled={!micEnabled}
              onChange={(v) => setMic({ volume: v })}
            />
          }
        />
      </Section>

      <Section
        title="Other sources"
        description={trackLayout === "single"
          ? "Applications currently producing audio. Mixed into the game audio track."
          : "Applications currently producing audio. Each records on its own track."}
      >
        {sessions.length === 0 && (
          <div class="set-field">
            <div class="set-field-help set-empty-row">No other applications are playing audio right now.</div>
          </div>
        )}
        {sessions.map((s) => {
          const src = processFor(s.process_name);
          const enabled = src?.enabled ?? false;
          const volume = src?.volume ?? 1;
          return (
            <Field
              key={s.process_id + s.process_name}
              label={appLabel(s.display_name, s.process_name)}
              control={
                <div class="audio-other-control">
                  <Toggle checked={enabled} onChange={(v) => setProcess(s, { enabled: v })} />
                  <VolumeSlider
                    value={volume}
                    disabled={!enabled}
                    onChange={(v) => setProcess(s, { volume: v })}
                  />
                </div>
              }
            />
          );
        })}
      </Section>
    </>
  );
}
