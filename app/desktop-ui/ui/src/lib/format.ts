// Display formatting helpers (deno.md §2.1).

export function formatSize(bytes: number): string {
  if (!bytes || bytes <= 0) return "—";
  const units = ["B", "KB", "MB", "GB", "TB"];
  let v = bytes;
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  // 1 decimal, automatic unit.
  return `${v.toFixed(1)} ${units[i]}`;
}

// Human-friendly application label from a raw process name. Raw executable
// names ("RocketLeague.exe") are never shown in the UI; when no catalog
// display name exists this derives one: strip the extension, split camelCase /
// separators, and title-case the words.
export function prettyAppName(processName?: string | null): string {
  if (!processName) return "Desktop";
  const stem = processName.replace(/\.[^./\\]+$/, "").split(/[/\\]/).pop() ?? "";
  const words = stem
    .replace(/[_\-.]+/g, " ")
    .replace(/([a-z\d])([A-Z])/g, "$1 $2")
    .replace(/([A-Z]+)([A-Z][a-z])/g, "$1 $2")
    .trim()
    .split(/\s+/)
    .filter(Boolean)
    .map((w) => (w === w.toLowerCase() ? w[0].toUpperCase() + w.slice(1) : w));
  return words.join(" ") || "Desktop";
}

// Resolves the label to show for an application: the catalog/engine display
// name when it's a real name, otherwise a prettified process name. Engine
// display names sometimes fall back to the raw executable ("chrome.exe"),
// which must never reach the UI.
export function appLabel(displayName?: string | null, processName?: string | null): string {
  const name = (displayName ?? "").trim();
  if (name && !/\.exe$/i.test(name)) return name;
  return prettyAppName(name || processName);
}

// Same as appLabel, but for clips specifically: when no game was detected,
// the label depends on how the clip was captured rather than falling back to
// the generic "Desktop" — a manual recording with no game is a screen
// recording, while a replay-buffer save with no game genuinely has none.
export function clipSourceLabel(
  clip: { source: "replay" | "manual"; game_display_name?: string | null; game_process_name?: string | null },
): string {
  if (clip.game_display_name || clip.game_process_name) {
    return appLabel(clip.game_display_name, clip.game_process_name);
  }
  return clip.source === "manual" ? "Screen Recording" : "None";
}

function pad(n: number): string {
  return n < 10 ? `0${n}` : String(n);
}

// Today 13:50 / Yesterday 13:50 / Jun 23 13:50 / Jun 23, 2025 13:50
export function formatDate(iso: string): string {
  const d = new Date(iso);
  if (isNaN(d.getTime())) return iso;
  const now = new Date();
  const time = `${pad(d.getHours())}:${pad(d.getMinutes())}`;

  const sameDay = (a: Date, b: Date) =>
    a.getFullYear() === b.getFullYear() &&
    a.getMonth() === b.getMonth() &&
    a.getDate() === b.getDate();

  const yesterday = new Date(now);
  yesterday.setDate(now.getDate() - 1);

  if (sameDay(d, now)) return `Today ${time}`;
  if (sameDay(d, yesterday)) return `Yesterday ${time}`;

  const month = d.toLocaleString("en-US", { month: "short" });
  if (d.getFullYear() === now.getFullYear()) {
    return `${month} ${d.getDate()} ${time}`;
  }
  return `${month} ${d.getDate()}, ${d.getFullYear()} ${time}`;
}

// Windows device paths ("\\.\DISPLAY2") are never user-facing copy — derive
// a friendly "Display N" label from the trailing device number instead.
export function monitorDisplayName(
  mon: { device?: string | null },
  index: number,
): string {
  const num = mon.device?.match(/(\d+)\s*$/)?.[1] ?? String(index + 1);
  return `Display ${num}`;
}

export function formatDuration(seconds: number | null): string {
  if (!seconds || seconds <= 0) return "";
  const m = Math.floor(seconds / 60);
  const s = Math.floor(seconds % 60);
  return `${m}:${pad(s)}`;
}
