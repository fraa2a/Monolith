import { useEffect, useRef, useState } from "preact/hooks";
import {
  exeIconUrl,
  fetchEngineStatus,
  fetchGameArtwork,
  type EngineStatus,
  type GameArtwork,
} from "../lib/api.ts";
import { appWindow } from "../lib/window.ts";
import { getConfig, getRuntimeStatus, saveConfig, type Config, type RuntimeStatus } from "../lib/settings-api.ts";
import { appLabel } from "../lib/format.ts";
import { Icon } from "./icons.tsx";

interface Props {
  view: string;
}

function cloneConfig(config: Config | null): Config {
  return JSON.parse(JSON.stringify(config ?? {}));
}

// "\\.\DISPLAY2" -> "Display 2". Device IDs are never user-facing copy.
function monitorLabel(mon: { device: string; width: number; height: number; primary: boolean }, index: number) {
  const num = mon.device?.match(/(\d+)\s*$/)?.[1] ?? String(index + 1);
  return `Display ${num}${mon.primary}`;
}

// Custom title bar (the window is decorations-less). The whole bar is a drag
// region except controls. Live capture status + manual-record control sit left,
// at a fixed offset from the brand, and fill the bar's full height.
export function Titlebar({ view }: Props) {
  const [runtime, setRuntime] = useState<RuntimeStatus>({});
  const [engine, setEngine] = useState<EngineStatus>({});
  const [config, setConfig] = useState<Config | null>(null);
  const [art, setArt] = useState<GameArtwork>({ icon: null, cover: null });
  const [exeIcon, setExeIcon] = useState<string | null>(null);
  const [open, setOpen] = useState(false);
  const [showConnectToast, setShowConnectToast] = useState(false);
  const hasCheckedOnce = useRef(false);

  // Runtime + engine status are read-only and safe to poll live.
  useEffect(() => {
    let alive = true;
    const load = async () => {
      const [rs, es] = await Promise.all([getRuntimeStatus(), fetchEngineStatus()]);
      if (!alive) return;
      setRuntime(rs);
      setEngine(es);
      if (!hasCheckedOnce.current) {
        hasCheckedOnce.current = true;
        if (es.connected === false) setShowConnectToast(true);
      } else if (es.connected !== false) {
        setShowConnectToast(false);
      }
    };
    load();
    const id = setInterval(load, 1600);
    return () => {
      alive = false;
      clearInterval(id);
    };
  }, []);

  // Config is loaded once, then refreshed only when the capture popover opens.
  // It is deliberately NOT polled: the titlebar itself is the writer, so a
  // periodic re-fetch would race the optimistic toggle and revert it a beat
  // later (the "sets then flips back" bug).
  useEffect(() => {
    let alive = true;
    getConfig().then((cfg) => {
      if (alive) setConfig(cfg);
    });
    return () => {
      alive = false;
    };
  }, []);

  useEffect(() => {
    if (!open) return;
    let alive = true;
    getConfig().then((cfg) => {
      if (alive) setConfig(cfg);
    });
    return () => {
      alive = false;
    };
  }, [open]);

  // Dismiss the capture popover on Escape or a click/mousedown outside it — the
  // same affordance the rest of the UI uses. Without this the popover could only
  // be closed by clicking the feed button again.
  useEffect(() => {
    if (!open) return;
    const onDown = (e: MouseEvent) => {
      const target = e.target as HTMLElement;
      if (!target.closest(".mode-popover, .capture-feed")) setOpen(false);
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") setOpen(false);
    };
    document.addEventListener("mousedown", onDown, true);
    document.addEventListener("keydown", onKey, true);
    return () => {
      document.removeEventListener("mousedown", onDown, true);
      document.removeEventListener("keydown", onKey, true);
    };
  }, [open]);

  const activeGame = runtime.active_game;
  const gameProcess = activeGame?.process_id ? activeGame.process_name : "";
  const exePath = activeGame?.process_id ? (activeGame.executable_path ?? "") : "";
  const gameName = activeGame?.process_id
    ? appLabel(activeGame.display_name, activeGame.process_name)
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

  // Prefer the icon embedded in the executable itself; fall back to the
  // catalog icon. exeIconUrl resolves to null when the path has no icon.
  useEffect(() => {
    let alive = true;
    if (!exePath) {
      setExeIcon(null);
      return;
    }
    exeIconUrl(exePath).then((dataUrl) => {
      if (alive) setExeIcon(dataUrl);
    });
    return () => {
      alive = false;
    };
  }, [exePath]);

  const mode = String(config?.capture_mode?.mode ?? "always");
  const autoRecord = Boolean(config?.capture_mode?.auto_record ?? false);
  const selectedMonitor = String(config?.capture?.monitor_device ?? "");
  const monitors = runtime.monitors ?? [];

  const connected = engine.connected !== false;
  const recording = !!engine.recording;
  const clipping = !recording && !!engine.replay_enabled;
  const statusLabel = !connected ? "Disconnected" : recording ? "Recording" : clipping ? "Clipping" : "Idle";
  const subject = !connected ? "No engine" : gameName || ((recording || clipping) ? "Screen" : "Ready");
  const patternIcon = exeIcon ?? art.icon ?? null;

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
    if ((e.target as HTMLElement).closest(".tb-btn, .capture-feed, .mode-popover")) return;
    appWindow.startDrag();
  };

  return (
    <>
      {showConnectToast && (
        <div class="connect-toast">
          <span>Couldn't reach the Monolith engine — is it running?</span>
          <button
            class="connect-toast-close"
            onClick={() => setShowConnectToast(false)}
            title="Dismiss"
          >
            <Icon name="x" size={14} />
          </button>
        </div>
      )}
      <div class="titlebar" onMouseDown={onMouseDown} onDblClick={() => appWindow.toggleMaximize()}>
        <div class="tb-brand">
          <span class="tb-app">MONOLITH</span>
          <span class="tb-sep">/</span>
          <span class="tb-view">{view}</span>
        </div>

        <div class="tb-status">
          <button
            class={`capture-feed ${patternIcon ? "" : "screen"}`}
            onMouseDown={(e) => e.stopPropagation()}
            onClick={(e) => {
              e.stopPropagation();
              setOpen((v) => !v);
            }}
            title="Capture source and mode"
          >
            <span
              class="feed-bg"
              aria-hidden="true"
              style={patternIcon ? { backgroundImage: `url(${patternIcon})` } : undefined}
            />
            <span class="feed-shade" aria-hidden="true" />
            <span class={`rec-dot ${!connected ? "disconnected" : recording ? "on" : clipping ? "clip" : ""}`} />
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

        <div class="tb-drag" />

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
    </>
  );
}
