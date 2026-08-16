// Updater window view. All state lives in the Rust host (src-tauri/src):
// the frontend is a pure projection of the "update-state" events plus a few
// commands (start / cancel / retry). Phases: checking → upToDate | available
// → downloading → applying → done | failed.

import { useEffect, useState } from "preact/hooks";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { openUrl } from "@tauri-apps/plugin-opener";

type Phase =
  | "checking"
  | "upToDate"
  | "available"
  | "downloading"
  | "applying"
  | "done"
  | "failed";

type CompStatus = "pending" | "downloading" | "verifying" | "ready" | "failed";

interface ComponentState {
  key: string; // "engine" | "ui" | "updater"
  from: string;
  to: string;
  size: number;
  downloaded: number;
  status: CompStatus;
}

interface InstalledVersions {
  engine: string;
  ui: string;
  updater: string;
}

interface UpdateState {
  phase: Phase;
  tag: string;
  notesUrl: string;
  publishedAt: string;
  error: string | null;
  recording: boolean;
  engineRunning: boolean;
  speedBps: number;
  components: ComponentState[];
  installed: InstalledVersions | null;
}

const COMPONENT_NAMES: Record<string, string> = {
  engine: "Engine",
  ui: "Interface",
  updater: "Updater",
};

const FALLBACK_INSTALLER_URL =
  "https://github.com/fraa2a/Monolith/releases/latest";

function formatBytes(bytes: number): string {
  if (bytes <= 0) return "0 B";
  const units = ["B", "KB", "MB", "GB"];
  const i = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1);
  const v = bytes / 1024 ** i;
  return `${v >= 100 || i === 0 ? Math.round(v) : v.toFixed(1)} ${units[i]}`;
}

function formatSpeed(bps: number): string {
  return bps > 0 ? `${formatBytes(bps)}/s` : "";
}

function formatDate(iso: string): string {
  if (!iso) return "";
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return "";
  return d.toLocaleDateString(undefined, {
    day: "numeric",
    month: "short",
    year: "numeric",
  });
}

function prettyTag(tag: string): string {
  return tag.replace(/^v/, "");
}

// ── Icons (inline SVG, same approach as the main app's shell/icons.tsx) ─────

function IconEngine() {
  return (
    <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
      <rect x="5" y="5" width="14" height="14" rx="2" />
      <rect x="9" y="9" width="6" height="6" />
      <path d="M9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3" />
    </svg>
  );
}

function IconInterface() {
  return (
    <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
      <rect x="3" y="4" width="18" height="16" rx="2" />
      <path d="M3 9h18" />
      <path d="M7 14h4M7 17h7" />
    </svg>
  );
}

function IconUpdater() {
  return (
    <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
      <path d="M21 12a9 9 0 1 1-2.6-6.4" />
      <path d="M21 3v6h-6" />
    </svg>
  );
}

function componentGlyph(key: string) {
  if (key === "engine") return <IconEngine />;
  if (key === "ui") return <IconInterface />;
  return <IconUpdater />;
}

function IconCheck() {
  return (
    <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
      <path d="M20 6 9 17l-5-5" />
    </svg>
  );
}

// ── Pieces ────────────────────────────────────────────────────────────────────

function Titlebar({ closable }: { closable: boolean }) {
  const win = getCurrentWindow();
  return (
    <div
      class="tb"
      onPointerDown={(e) => {
        if (e.button === 0) win.startDragging();
      }}
    >
      <div class="tb-mark" />
      <div class="tb-title">Monolith Updater</div>
      <div class="tb-drag" />
      <button
        class="tb-close"
        title={closable ? "Close" : "Update in progress…"}
        disabled={!closable}
        onClick={() => win.close()}
      >
        ✕
      </button>
    </div>
  );
}

function VersionLine({ c }: { c: ComponentState }) {
  const from = c.from === "0.0.0" ? "–" : c.from;
  return (
    <div class="comp-vers">
      <span class="from">{from}</span>
      <span class="arr">→</span>
      <span class="to">{c.to}</span>
      {c.status === "pending" && c.size > 0 && (
        <span class="size">{formatBytes(c.size)}</span>
      )}
    </div>
  );
}

function ComponentCard({ c }: { c: ComponentState }) {
  const pct = c.size > 0 ? Math.min(100, (c.downloaded / c.size) * 100) : 0;
  const showMeter =
    c.status === "downloading" || c.status === "verifying" || c.status === "ready";
  const statusText =
    c.status === "downloading"
      ? `${formatBytes(c.downloaded)} / ${formatBytes(c.size)}`
      : c.status === "verifying"
        ? "verifying signature…"
        : c.status === "ready"
          ? "verified · ready"
          : c.status === "failed"
            ? "failed"
            : "";
  return (
    <div
      class={`comp${c.status === "downloading" || c.status === "verifying" ? " active" : ""}${c.status === "failed" ? " fail" : ""}`}
    >
      <div class="comp-top">
        <div class="comp-glyph">{componentGlyph(c.key)}</div>
        <div>
          <div class="comp-name">{COMPONENT_NAMES[c.key] ?? c.key}</div>
          <div class="comp-sub">
            {c.status === "ready" ? "downloaded" : "component update"}
          </div>
        </div>
        <VersionLine c={c} />
      </div>
      {showMeter && (
        <>
          <div class="meter">
            <i style={{ width: `${c.status === "ready" ? 100 : pct}%` }} />
          </div>
          <div class="comp-row">
            <span class={`stat${c.status === "ready" ? " ok" : ""}`}>{statusText}</span>
            <span>{COMPONENT_NAMES[c.key] ?? c.key}</span>
          </div>
        </>
      )}
    </div>
  );
}

function CheckingState() {
  return (
    <div class="center">
      <div class="dots">
        <i />
        <i />
        <i />
      </div>
      <div class="center-title">Checking for updates</div>
      <div class="center-sub">Reading the release manifest…</div>
    </div>
  );
}

function UpToDateState({ installed }: { installed: InstalledVersions | null }) {
  return (
    <div class="center">
      <div class="center-glyph">
        <IconCheck />
      </div>
      <div class="center-title">Monolith is up to date</div>
      <div class="center-sub">All components match the latest release.</div>
      {installed && (
        <div class="center-vers">
          <span>
            engine <b>{installed.engine}</b>
          </span>
          <span>
            interface <b>{installed.ui}</b>
          </span>
          <span>
            updater <b>{installed.updater}</b>
          </span>
        </div>
      )}
    </div>
  );
}

function ApplyingState({ engineUpdating }: { engineUpdating: boolean }) {
  return (
    <div class="center">
      <div class="sweep" />
      <div class="center-title">Applying update</div>
      <div class="center-sub">
        {engineUpdating
          ? "Monolith restarts automatically when the new engine is in place."
          : "Replacing components on disk…"}
      </div>
    </div>
  );
}

function DoneState({ engineUpdated }: { engineUpdated: boolean }) {
  return (
    <div class="center">
      <div class="center-glyph">
        <IconCheck />
      </div>
      <div class="center-title">Update complete</div>
      <div class="center-sub">
        {engineUpdated
          ? "Monolith has been restarted on the new version."
          : "The new interface is active next time you open Monolith."}
      </div>
    </div>
  );
}

function ErrorState({ message, onRetry }: { message: string; onRetry: () => void }) {
  return (
    <div class="upd-body">
      <div class="error-box">
        <span class="title">Update failed</span>
        <span>{message}</span>
      </div>
      <div class="comps-label">Alternatives</div>
      <div class="comps">
        <div class="comp">
          <div class="comp-top">
            <div class="comp-glyph">
              <IconUpdater />
            </div>
            <div>
              <div class="comp-name">Full installer</div>
              <div class="comp-sub">replaces every component at once</div>
            </div>
          </div>
        </div>
      </div>
      <button
        class="btn btn-ghost"
        style={{ alignSelf: "flex-start" }}
        onClick={() => openUrl(FALLBACK_INSTALLER_URL)}
      >
        Open releases page
      </button>
      <div style={{ flex: 1 }} />
      <button class="btn btn-accent" style={{ alignSelf: "flex-end" }} onClick={onRetry}>
        Try again
      </button>
    </div>
  );
}

// ── Buffer bar (signature): the window fills like the replay buffer ──────────

function BufferBar({ state }: { state: UpdateState }) {
  const total = state.components.reduce((acc, c) => acc + c.size, 0);
  const done = state.components.reduce((acc, c) => acc + Math.min(c.downloaded, c.size), 0);
  const pct = total > 0 ? Math.min(100, (done / total) * 100) : 0;
  const live = state.phase === "downloading";

  let label = "STANDBY";
  let fill = 0;
  if (state.phase === "checking") label = "SCANNING";
  else if (state.phase === "available") {
    label = total > 0 ? `READY · ${formatBytes(total)}` : "READY";
    fill = 0;
  } else if (state.phase === "downloading") {
    label = `${Math.round(pct)}% · ${formatBytes(done)} / ${formatBytes(total)}`;
    fill = pct;
  } else if (state.phase === "applying") {
    label = "APPLYING";
    fill = 100;
  } else if (state.phase === "done") {
    label = "COMPLETE";
    fill = 100;
  } else if (state.phase === "upToDate") label = "UP TO DATE";
  else if (state.phase === "failed") label = "HALTED";

  return (
    <div class={`buffer${live ? " live" : ""}`}>
      <span>{label}</span>
      <div class="buffer-track">
        <div class="buffer-fill" style={{ width: `${fill}%` }} />
      </div>
      {live && <span>{formatSpeed(state.speedBps)}</span>}
    </div>
  );
}

// ── Root ─────────────────────────────────────────────────────────────────────

export function Updater() {
  const [state, setState] = useState<UpdateState | null>(null);

  useEffect(() => {
    invoke<UpdateState>("updater_state").then(setState).catch(() => {});
    const un = listen<UpdateState>("update-state", (e) => setState(e.payload));
    return () => {
      un.then((f) => f());
    };
  }, []);

  if (!state) {
    return (
      <>
        <Titlebar closable />
        <CheckingState />
        <BufferBar
          state={{
            phase: "checking",
            tag: "",
            notesUrl: "",
            publishedAt: "",
            error: null,
            recording: false,
            engineRunning: false,
            speedBps: 0,
            components: [],
            installed: null,
          }}
        />
      </>
    );
  }

  const phase = state.phase;
  const engineUpdating = state.components.some((c) => c.key === "engine");
  const totalSize = state.components.reduce((acc, c) => acc + c.size, 0);
  const closable = phase !== "applying" && phase !== "downloading";

  const start = () => invoke("updater_start").catch(() => {});
  const cancel = () => invoke("updater_cancel").catch(() => {});
  const retry = () => invoke("updater_retry").catch(() => {});

  let body;
  if (phase === "checking") body = <CheckingState />;
  else if (phase === "upToDate") body = <UpToDateState installed={state.installed} />;
  else if (phase === "applying") body = <ApplyingState engineUpdating={engineUpdating} />;
  else if (phase === "done") body = <DoneState engineUpdated={engineUpdating} />;
  else if (phase === "failed")
    body = <ErrorState message={state.error ?? "Unknown error."} onRetry={retry} />;
  else {
    // available | downloading
    const downloading = phase === "downloading";
    body = (
      <div class="upd-body">
        <div class="release">
          <div class="release-tag">
            Version <span class="v">{prettyTag(state.tag)}</span>
          </div>
          <div class="release-meta">
            {state.publishedAt && <span>{formatDate(state.publishedAt)}</span>}
            {state.publishedAt && <span class="dot" />}
            {state.notesUrl ? (
              <a
                href={state.notesUrl}
                onClick={(e) => {
                  e.preventDefault();
                  openUrl(state.notesUrl);
                }}
              >
                Release notes
              </a>
            ) : (
              <span>component update</span>
            )}
          </div>
        </div>

        <div class="comps">
          <div class="comps-label">{downloading ? "Downloading" : "Updates"}</div>
          {state.components.map((c) => (
            <ComponentCard key={c.key} c={c} />
          ))}
        </div>

        {state.recording && phase === "available" && (
          <div class="notice">
            <span>●</span>
            <span>
              A recording is in progress. Stop it before updating — the engine
              restarts to apply engine updates.
            </span>
          </div>
        )}

        <div class="total">
          <span>Total download</span>
          <strong>{formatBytes(totalSize)}</strong>
        </div>

        <div style={{ flex: 1 }} />
      </div>
    );
  }

  // Footer varies per phase; the available/downloading phases keep the
  // primary actions here so the body stays a pure projection of state.
  let foot;
  if (phase === "checking" || phase === "applying" || phase === "failed") {
    foot = null; // ErrorState renders its own actions inline.
  } else if (phase === "upToDate" || phase === "done") {
    foot = (
      <div class="foot">
        <span class="foot-note">
          {phase === "done" ? "update applied" : "no action needed"}
        </span>
        <button class="btn btn-accent" onClick={() => getCurrentWindow().close()}>
          Done
        </button>
      </div>
    );
  } else if (phase === "downloading") {
    foot = (
      <div class="foot">
        <span class="foot-note">
          {state.speedBps > 0 ? formatSpeed(state.speedBps) : "…"}
        </span>
        <button class="btn btn-ghost" onClick={cancel}>
          Cancel
        </button>
      </div>
    );
  } else {
    foot = (
      <div class="foot">
        <span class="foot-note">
          {state.engineRunning ? "engine online" : "engine offline"}
        </span>
        <button class="btn btn-ghost" onClick={() => getCurrentWindow().close()}>
          Later
        </button>
        <button class="btn btn-accent" disabled={state.recording} onClick={start}>
          Update now
        </button>
      </div>
    );
  }

  return (
    <>
      <Titlebar closable={closable} />
      {body}
      {foot}
      <BufferBar state={state} />
    </>
  );
}
