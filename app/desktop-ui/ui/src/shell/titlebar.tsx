import { useEffect, useMemo, useState } from "preact/hooks";
import { fetchEngineStatus, fetchGameArtwork, type EngineStatus, type GameArtwork } from "../lib/api.ts";
import { appWindow } from "../lib/window.ts";
import { getConfig, getRuntimeStatus, saveConfig, type Config, type RuntimeStatus } from "../lib/settings-api.ts";
import { Icon } from "./icons.tsx";

interface Props {
  view: string;
}

function cloneConfig(config: Config | null): Config {
  return JSON.parse(JSON.stringify(config ?? {}));
}

function monitorLabel(mon: { device: string; width: number; height: number; primary: boolean }, index: number) {
  const name = mon.device?.replace(/^\\\\\.\\/, "") || `Display ${index + 1}`;
  return `${name}${mon.primary ? " Primary" : ""}`;
}

// Custom title bar (the window is decorations-less). The whole bar is a drag
// region except controls. The recorder feed is the compact live status + mode selector.
export function Titlebar({ view }: Props) {
  const [runtime, setRuntime] = useState<RuntimeStatus>({});
  const [engine, setEngine] = useState<EngineStatus>({});
  const [config, setConfig] = useState<Config | null>(null);
  const [art, setArt] = useState<GameArtwork>({ icon: null, cover: null });
  const [open, setOpen] = useState(false);

  useEffect(() => {
    let alive = true;
    const load = async () => {
      const [rs, es, cfg] = await Promise.all([getRuntimeStatus(), fetchEngineStatus(), getConfig()]);
      if (!alive) return;
      setRuntime(rs);
      setEngine(es);
      setConfig(cfg);
    };
    load();
    const id = setInterval(load, 1600);
    return () => {
      alive = false;
      clearInterval(id);
    };
  }, []);

  const activeGame = runtime.active_game;
  const gameProcess = activeGame?.process_id ? activeGame.process_name : "";
  const gameName = activeGame?.process_id
    ? (activeGame.display_name || activeGame.process_name || "Game")
    : "";

  useEffect(() => {
    let alive = true;
    if (!gameProcess) {
      setArt({ icon: null, cover: null });
      return;
    }
    fetchGameArtwork({ game_process_name: gameProcess }).then((next) => {
      if (alive) setArt(next);
    });
    return () => {
      alive = false;
    };
  }, [gameProcess]);

  const mode = String(config?.capture_mode?.mode ?? "always");
  const autoRecord = Boolean(config?.capture_mode?.auto_record ?? false);
  const selectedMonitor = String(config?.capture?.monitor_device ?? "");
  const monitors = runtime.monitors ?? [];

  const statusLabel = engine.recording ? "Recording" : engine.replay_enabled ? "Clipping" : "Idle";
  const subject = gameName || ((engine.recording || engine.replay_enabled) ? "Screen" : "Ready");
  const isScreen = !gameName;

  const feedStyle = useMemo(() => {
    if (!art.cover) return undefined;
    return { backgroundImage: `linear-gradient(90deg, rgba(7,7,7,.9), rgba(7,7,7,.62)), url(${art.cover})` };
  }, [art.cover]);

  const persist = async (mutate: (draft: Config) => void) => {
    const draft = cloneConfig(config);
    draft.capture_mode ??= {};
    draft.capture ??= {};
    mutate(draft);
    setConfig(draft);
    await saveConfig(draft);
  };

  const setMode = (next: "always" | "game_only") => {
    persist((draft) => {
      draft.capture_mode.mode = next;
      if (next === "game_only") draft.active_game = { ...(draft.active_game ?? {}), detection_enabled: true };
    });
  };

  const setMonitor = (device: string) => {
    persist((draft) => {
      draft.capture.monitor_device = device;
    });
  };

  const setAutoRecord = (value: boolean) => {
    persist((draft) => {
      draft.capture_mode.auto_record = value;
    });
  };

  const onMouseDown = (e: MouseEvent) => {
    if (e.button !== 0) return;
    if ((e.target as HTMLElement).closest(".tb-btn, .recorder-feed, .mode-popover")) return;
    appWindow.startDrag();
  };

  return (
    <div class="titlebar" onMouseDown={onMouseDown} onDblClick={() => appWindow.toggleMaximize()}>
      <div class="tb-drag">
        <span class="tb-app">MONOLITH</span>
        <span class="tb-sep">/</span>
        <span class="tb-view">{view}</span>
      </div>

      <div class="recorder-wrap">
        <button
          class={`recorder-feed ${isScreen ? "screen" : "game"}`}
          style={feedStyle}
          onMouseDown={(e) => e.stopPropagation()}
          onClick={(e) => {
            e.stopPropagation();
            setOpen((v) => !v);
          }}
          title="Recording mode"
        >
          <span class={`rec-dot ${engine.recording ? "on" : engine.replay_enabled ? "clip" : ""}`} />
          <span class="rec-copy">
            <span class="rec-state">{statusLabel}</span>
            <span class="rec-subject">{subject}</span>
          </span>
        </button>

        {open && (
          <div class="mode-popover" onMouseDown={(e) => e.stopPropagation()}>
            <div class="segmented mode-tabs">
              <button class={`seg ${mode === "game_only" ? "active" : ""}`} onClick={() => setMode("game_only")}>
                <Icon name="gamepad" size={15} />
                Game Recording
              </button>
              <button class={`seg ${mode !== "game_only" ? "active" : ""}`} onClick={() => setMode("always")}>
                <Icon name="monitor" size={15} />
                Screen Recording
              </button>
            </div>

            {mode === "game_only"
              ? (
                <div class="mode-body">
                  <div class="mode-row">
                    <div>
                      <div class="mode-label">Auto Record</div>
                      <div class="mode-help">Starts when a supported game appears and stops when it exits.</div>
                    </div>
                    <button class={`toggle ${autoRecord ? "on" : ""}`} onClick={() => setAutoRecord(!autoRecord)} title="Auto Record">
                      <span class="toggle-knob" />
                    </button>
                  </div>
                  {!gameName && <div class="mode-warning">No supported game detected.</div>}
                </div>
              )
              : (
                <div class="monitor-grid">
                  {monitors.map((mon, i) => {
                    const active = selectedMonitor ? selectedMonitor === mon.device : mon.primary;
                    const ratio = mon.width && mon.height ? `${mon.width} / ${mon.height}` : "16 / 9";
                    return (
                      <button class={`monitor-card ${active ? "active" : ""}`} onClick={() => setMonitor(mon.device)} key={mon.device || i}>
                        <span class="monitor-preview" style={{ aspectRatio: ratio }}>
                          <Icon name="monitor" size={20} />
                        </span>
                        <span class="monitor-name">{monitorLabel(mon, i)}</span>
                        <span class="monitor-size">{mon.width} x {mon.height}</span>
                      </button>
                    );
                  })}
                </div>
              )}
          </div>
        )}
      </div>

      <div class="tb-controls">
        <button class="tb-btn" title="Minimize" onClick={() => appWindow.minimize()}>
          <svg width="11" height="11" viewBox="0 0 11 11" aria-hidden="true">
            <rect x="1" y="5.5" width="9" height="1" fill="currentColor" />
          </svg>
        </button>
        <button class="tb-btn" title="Maximize" onClick={() => appWindow.toggleMaximize()}>
          <svg width="11" height="11" viewBox="0 0 11 11" aria-hidden="true">
            <rect x="1.5" y="1.5" width="8" height="8" fill="none" stroke="currentColor" stroke-width="1" />
          </svg>
        </button>
        <button class="tb-btn tb-close" title="Close" onClick={() => appWindow.close()}>
          <svg width="11" height="11" viewBox="0 0 11 11" aria-hidden="true">
            <path d="M1.5 1.5l8 8M9.5 1.5l-8 8" stroke="currentColor" stroke-width="1.1" />
          </svg>
        </button>
      </div>
    </div>
  );
}
