import type { ComponentChildren } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { Icon } from "../shell/icons.tsx";
import { pickFolder } from "../lib/settings-api.ts";

// ── Layout primitives ──────────────────────────────────────────────────────────
// A settings page is a stack of Sections; each Section is a titled card holding
// a set of Fields. This gives every group a consistent frame, divider and rhythm.

export function Section(
  { title, description, children }: {
    title: string;
    description?: string;
    children: ComponentChildren;
  },
) {
  return (
    <section class="set-section">
      <div class="set-section-head">
        <h3 class="set-section-title">{title}</h3>
        {description && <p class="set-section-desc">{description}</p>}
      </div>
      <div class="set-card">{children}</div>
    </section>
  );
}

// A single labelled row: label + optional help on the left, the control on the
// right. Rows are separated by hairline dividers inside a card.
export function Field(
  { label, help, control, htmlFor }: {
    label: string;
    help?: string;
    control: ComponentChildren;
    htmlFor?: string;
  },
) {
  return (
    <div class="set-field">
      <div class="set-field-label">
        <label for={htmlFor}>{label}</label>
        {help && <span class="set-field-help">{help}</span>}
      </div>
      <div class="set-field-control">{control}</div>
    </div>
  );
}

// ── Controls ────────────────────────────────────────────────────────────────────

export function Toggle(
  { checked, onChange }: { checked: boolean; onChange: (v: boolean) => void },
) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      class={`toggle ${checked ? "on" : ""}`}
      onClick={() => onChange(!checked)}
    >
      <span class="toggle-knob" />
    </button>
  );
}

export function Select(
  { value, options, onChange, disabled }: {
    value: string;
    options: { value: string; label: string; danger?: boolean }[];
    onChange: (v: string) => void;
    disabled?: boolean;
  },
) {
  return (
    <select
      class="select"
      value={value}
      disabled={disabled}
      onChange={(e) => onChange((e.target as HTMLSelectElement).value)}
    >
      {options.map((o) => (
        <option value={o.value} key={o.value} class={o.danger ? "opt-danger" : ""}>
          {o.label}
        </option>
      ))}
    </select>
  );
}

export function TextInput(
  { value, onInput, placeholder, disabled, type = "text", min, max }: {
    value: string | number;
    onInput: (v: string) => void;
    placeholder?: string;
    disabled?: boolean;
    type?: string;
    min?: number;
    max?: number;
  },
) {
  return (
    <input
      class="input"
      type={type}
      min={min}
      max={max}
      value={value}
      disabled={disabled}
      placeholder={placeholder}
      onInput={(e) => onInput((e.target as HTMLInputElement).value)}
    />
  );
}

// Read-only path field with a Browse button that opens the native Windows folder
// picker. The user never types a path manually.
export function FolderPicker(
  { value, onPick }: { value: string; onPick: (path: string) => void },
) {
  const [busy, setBusy] = useState(false);
  const browse = async () => {
    if (busy) return;
    setBusy(true);
    const chosen = await pickFolder(value);
    setBusy(false);
    if (chosen) onPick(chosen);
  };
  return (
    <div class="folder-picker">
      <input class="input folder-path" value={value} readOnly title={value} />
      <button type="button" class="btn btn-ghost" onClick={browse} disabled={busy}>
        <Icon name="folder-open" size={15} />
        <span>Browse</span>
      </button>
    </div>
  );
}

// Segmented control (e.g. CPU / GPU). Compact, mutually-exclusive options.
export function Segmented(
  { value, options, onChange }: {
    value: string;
    options: { value: string; label: string; icon?: string }[];
    onChange: (v: string) => void;
  },
) {
  return (
    <div class="segmented" role="tablist">
      {options.map((o) => (
        <button
          key={o.value}
          type="button"
          role="tab"
          aria-selected={value === o.value}
          class={`seg ${value === o.value ? "active" : ""}`}
          onClick={() => onChange(o.value)}
        >
          {o.icon && <Icon name={o.icon} size={15} />}
          <span>{o.label}</span>
        </button>
      ))}
    </div>
  );
}

// A slider bound to a 0–100 percentage, showing the live value.
export function VolumeSlider(
  { value, onChange, disabled }: {
    value: number; // 0..1
    onChange: (v: number) => void; // 0..1
    disabled?: boolean;
  },
) {
  const pct = Math.round((value ?? 1) * 100);
  return (
    <div class={`volume ${disabled ? "disabled" : ""}`}>
      <input
        type="range"
        min={0}
        max={100}
        value={pct}
        disabled={disabled}
        onInput={(e) => onChange(Number((e.target as HTMLInputElement).value) / 100)}
      />
      <span class="volume-value">{pct}%</span>
    </div>
  );
}

// ── Hotkey capture ──────────────────────────────────────────────────────────────
// Replaces free-text hotkey entry. On focus it listens for a real key chord and
// writes the canonical "Ctrl+Alt+3" string, so combinations the old text box
// dropped (e.g. Ctrl+Alt+3) are captured correctly.

const MOD_LABEL: Record<string, string> = {
  Control: "Ctrl",
  Shift: "Shift",
  Alt: "Alt",
  Meta: "Win",
};

function keyLabel(e: KeyboardEvent): string | null {
  const k = e.key;
  if (k === "Control" || k === "Shift" || k === "Alt" || k === "Meta") return null;
  if (k === " ") return "Space";
  if (k.startsWith("Arrow")) return k.slice(5); // Left/Right/Up/Down
  if (k.length === 1) return k.toUpperCase();
  // Named keys (F1–F24, Enter, Escape, Tab, Delete, Home, …) pass through as-is.
  return k.charAt(0).toUpperCase() + k.slice(1);
}

export function HotkeyCapture(
  { value, onChange, invalid }: { value: string; onChange: (v: string) => void; invalid?: boolean },
) {
  const [capturing, setCapturing] = useState(false);
  const ref = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    if (!capturing) return;
    const onKey = (e: KeyboardEvent) => {
      e.preventDefault();
      e.stopPropagation();
      // Escape / Backspace / Delete all clear the binding: the shortcut is set to
      // "NONE", which the engine treats as disabled (skips RegisterHotKey).
      if (e.key === "Escape" || e.key === "Backspace" || e.key === "Delete") {
        onChange("NONE");
        setCapturing(false);
        ref.current?.blur();
        return;
      }
      const main = keyLabel(e);
      if (!main) return; // modifier alone — wait for a real key
      const parts: string[] = [];
      if (e.ctrlKey) parts.push(MOD_LABEL.Control);
      if (e.shiftKey) parts.push(MOD_LABEL.Shift);
      if (e.altKey) parts.push(MOD_LABEL.Alt);
      if (e.metaKey) parts.push(MOD_LABEL.Meta);
      parts.push(main);
      onChange(parts.join("+"));
      setCapturing(false);
      ref.current?.blur();
    };
    globalThis.addEventListener("keydown", onKey, true);
    return () => globalThis.removeEventListener("keydown", onKey, true);
  }, [capturing, onChange]);

  const display = value === "NONE" || !value ? "Not set" : value;
  return (
    <button
      ref={ref}
      type="button"
      class={`hotkey-capture ${capturing ? "capturing" : ""} ${invalid ? "invalid" : ""}`}
      onClick={() => setCapturing(true)}
      onBlur={() => setCapturing(false)}
    >
      <Icon name="keyboard" size={15} />
      <span class="hotkey-value">
        {capturing ? "Press a combination… (Esc to clear)" : display}
      </span>
    </button>
  );
}
