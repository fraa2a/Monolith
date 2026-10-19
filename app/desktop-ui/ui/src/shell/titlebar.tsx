import { useEffect, useRef, useState } from "preact/hooks";
import {
  exeIconUrl,
  fetchEngineStatus,
  fetchGameArtwork,
  setSelectedGame,
  recorderCommand,
  type RecorderCommand,
  type EngineStatus,
  type GameArtwork,
} from "../lib/api.ts";
import { appWindow } from "../lib/window.ts";
import { getConfig, getRuntimeStatus, editConfig, settingsDraft, settingsError, type Config, type RuntimeStatus } from "../lib/settings-api.ts";
import { appLabel, monitorDisplayName } from "../lib/format.ts";
import { Icon } from "./icons.tsx";
import { Button } from "../components/ui/button.tsx";

interface Props {
  view: string;
  settingsActive: boolean;
}

// Native dragging excludes interactive controls.

export function Titlebar({ view, settingsActive }: Props) {
  const [runtime, setRuntime] = useState<RuntimeStatus>({});
  const [engine, setEngine] = useState<EngineStatus>({});
  const config = settingsDraft.value;
  const [art, setArt] = useState<GameArtwork>({ icon: null, cover: null });
  const [exeIcon, setExeIcon] = useState<string | null>(null);
  const [open, setOpen] = useState(false);
  const [showConnectToast, setShowConnectToast] = useState(false);
  const hasCheckedOnce = useRef(false);
  const [saveError, setSaveError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    let alive = true;
    const load = async () => {
      // The Rust watcher keeps the engine connection alive; skip the
      // runtime-status read + TCP poll while the window is hidden.
      if (document.hidden) return;
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

  useEffect(() => { void getConfig(); }, []);

  // Dismiss the capture popover on Escape or a click/mousedown outside it - the
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
    exeIconUrl(exePath, gameProcess).then((dataUrl) => {
      if (alive) setExeIcon(dataUrl);
    });
    return () => {
      alive = false;
    };
  }, [exePath, gameProcess]);

  const mode = String(config?.capture_mode?.mode ?? "always");
  const autoRecord = Boolean(config?.capture_mode?.auto_record ?? false);
  const clipWithoutGame = Boolean(config?.capture_mode?.clip_without_game ?? false);
  const selectedMonitor = String(config?.capture?.monitor_device ?? "");
  const monitors = runtime.monitors ?? [];
  const candidates = runtime.game_candidates ?? [];
  const selectedGamePid = runtime.selected_game_pid ?? 0;

  const connected = engine.connected !== false;
  const recording = !!engine.recording;
  const clipping = !recording && !!engine.replay_running;
  const statusLabel = !connected ? "Disconnected" : engine.recording_error ? "Recording error" : recording ? "Recording" : clipping ? "Clipping" : "Idle";
  const gameLabel = activeGame?.process_id
    ? appLabel(activeGame.display_name, activeGame.process_name)
    : "";
  const subject = !connected ? "No engine" : (gameLabel || ((recording || clipping) ? "Screen" : "Ready"));
  const patternIcon = exeIcon ?? art.icon ?? null;

  const persist = (mutate: (draft: Config) => void) => {
    editConfig((draft) => {
      draft.capture_mode ??= {};
      draft.capture ??= {};
      mutate(draft);
    }, true);
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

  const setClipWithoutGame = (value: boolean) => {
    persist((draft) => {
      draft.capture_mode.clip_without_game = value;
    });
  };

  // Selecting a game is engine runtime state (not settings): it takes effect
  // immediately via IPC and is reflected on the next runtime-status poll.
  const pickGame = (exe: string) => {
    void setSelectedGame(exe);
  };

  const record = async (method: RecorderCommand) => {
    if (busy) return;
    setBusy(true);
    setSaveError(null);
    const result = await recorderCommand(method);
    if (!result.ok) setSaveError(result.error ?? "Recorder command failed");
    else setEngine(await fetchEngineStatus());
    setBusy(false);
  };

  const onMouseDown = (e: MouseEvent) => {
    if (e.button !== 0) return;
    if ((e.target as HTMLElement).closest("button, input, select, .mode-popover")) return;
    appWindow.startDrag();
  };

  return (
    <>
      {(saveError || settingsError.value || engine.recording_error) && (
        <div class="connect-toast" role="alert">
          <span>{saveError || settingsError.value || engine.recording_error}</span>
          <button class="connect-toast-close" onClick={() => { setSaveError(null); settingsError.value = null; }} title="Dismiss">×</button>
        </div>
      )}
      {showConnectToast && (
        <div class="connect-toast">
          <span>Couldn't reach the Monolith engine, is it running?</span>
          <button
            class="connect-toast-close"
            onClick={() => setShowConnectToast(false)}
            title="Dismiss"
          >
            <Icon name="x" size={14} />
          </button>
        </div>
      )}
      <div class="titlebar" onMouseDown={onMouseDown} onDblClick={(e) => {
        if (!(e.target as HTMLElement).closest("button, input, select, .mode-popover")) appWindow.toggleMaximize();
      }}>
        <div class="tb-brand">
          <span class="tb-app" aria-label="Monolith">M</span>
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
            aria-expanded={open}
            aria-label={`Capture source: ${statusLabel}, ${subject}`}
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
                    {candidates.length > 1 && (
                      <div class="game-picker">
                        <div class="mode-label">Recording</div>
                        <div class="game-pick-list">
                          {candidates.map((c) => (
                            <button
                              key={c.process_id}
                              class={`game-pick ${c.process_id === selectedGamePid ? "active" : ""}`}
                              onClick={() => pickGame(c.process_name)}
                              title={appLabel(c.display_name, c.process_name)}
                            >
                              {appLabel(c.display_name, c.process_name)}
                            </button>
                          ))}
                          <button class="game-pick auto" onClick={() => pickGame("auto")} title="Automatic (most recently focused)">
                            Auto
                          </button>
                        </div>
                      </div>
                    )}
                    <div class="mode-row">
                      <div>
                        <div class="mode-label">Auto Record</div>
                        <div class="mode-help">Starts when a supported game appears and stops when it exits.</div>
                      </div>
                      <button class={`toggle ${autoRecord ? "on" : ""}`} onClick={() => setAutoRecord(!autoRecord)} title="Auto Record">
                        <span class="toggle-knob" />
                      </button>
                    </div>
                    <div class="mode-row">
                      <div>
                        <div class="mode-label">Clip Without Game</div>
                        <div class="mode-help">Keep clipping the full screen even when no game is detected.</div>
                      </div>
                      <button class={`toggle ${clipWithoutGame ? "on" : ""}`} onClick={() => setClipWithoutGame(!clipWithoutGame)} title="Clip Without Game">
                        <span class="toggle-knob" />
                      </button>
                    </div>
                    {!activeGame && <div class="mode-warning">No supported game detected.</div>}
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
                          <span class="monitor-name">{monitorDisplayName(mon, i)}</span>
                          <span class="monitor-size">{mon.width} x {mon.height}</span>
                        </button>
                      );
                    })}
                  </div>
                )}
            </div>
          )}
        </div>

        <div class="capture-actions">
          <Button variant="ghost" size="sm" disabled={busy || engine.connected !== true || !engine.replay_enabled}
            onClick={() => record("save_replay")} title="Save an instant replay">
            <kbd>{config?.hotkeys?.save_replay ?? "Ctrl+Shift+F8"}</kbd>
            <span>Clip {Number(config?.replay_buffer?.duration_seconds ?? 30)}s</span>
          </Button>
          <Button variant="ghost" size="sm" class={recording ? "record-active" : ""}
            disabled={busy || engine.connected !== true || (!recording && !config?.recording?.enabled)}
            onClick={() => record(recording ? "recording_stop" : "recording_start")}>
            <kbd>{recording ? (config?.hotkeys?.recording_stop ?? "Ctrl+Shift+F10") : (config?.hotkeys?.recording_start ?? "Ctrl+Shift+F9")}</kbd>
            <span>{recording ? "Stop recording" : "Long recording"}</span>
          </Button>
        </div>

        <div class="tb-drag" />
        <span class="tb-view">{view}</span>

        <div class="tb-controls">
          <button class="tb-btn" title="Minimize" aria-label="Minimize" onClick={() => appWindow.minimize()}>
            <Icon name="window-minimize" size={16} />
          </button>
          <button class="tb-btn" title="Maximize" aria-label="Maximize" onClick={() => appWindow.toggleMaximize()}>
            <Icon name="window-maximize" size={14} />
          </button>
          <button class="tb-btn tb-close" title="Close" aria-label="Close" onClick={() => appWindow.close()}>
            <Icon name="x" size={16} />
          </button>
        </div>
      </div>
    </>
  );
}
