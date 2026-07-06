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

export function formatDuration(seconds: number | null): string {
  if (!seconds || seconds <= 0) return "";
  const m = Math.floor(seconds / 60);
  const s = Math.floor(seconds % 60);
  return `${m}:${pad(s)}`;
}
