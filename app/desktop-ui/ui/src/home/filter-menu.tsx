import { useEffect, useRef, useState } from "preact/hooks";
import { Icon } from "../shell/icons.tsx";

interface Props {
  icon: string;
  title: string;                 // tooltip / aria-label for the trigger
  placeholder: string;           // search-box placeholder
  allLabel: string;              // the "clear filter" row, e.g. "All games"
  options: string[];             // values to choose from (games / hashtags)
  value?: string;                // currently selected value (undefined = none)
  onChange: (v: string | undefined) => void;
  format?: (o: string) => string; // display transform (e.g. "#" + tag)
}

// White-icon filter trigger that opens a custom dropdown: a type-to-search box
// plus the filterable option list. Replaces the native <select> so games and
// hashtags can be searched, not just scrolled.
export function FilterMenu(
  { icon, title, placeholder, allLabel, options, value, onChange, format }: Props,
) {
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState("");
  const ref = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLInputElement>(null);

  // Close on outside click / Escape, and focus the search box on open.
  useEffect(() => {
    if (!open) {
      setQuery("");
      return;
    }
    const onDown = (e: MouseEvent) => {
      if (!ref.current?.contains(e.target as Node)) setOpen(false);
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") setOpen(false);
    };
    document.addEventListener("mousedown", onDown, true);
    document.addEventListener("keydown", onKey, true);
    inputRef.current?.focus();
    return () => {
      document.removeEventListener("mousedown", onDown, true);
      document.removeEventListener("keydown", onKey, true);
    };
  }, [open]);

  const q = query.trim().toLowerCase();
  const filtered = q ? options.filter((o) => o.toLowerCase().includes(q)) : options;
  const active = value != null && value !== "";

  const pick = (v: string | undefined) => {
    onChange(v);
    setOpen(false);
  };

  return (
    <div class="filter-menu" ref={ref}>
      <button
        class={`filter-btn${active ? " active" : ""}${open ? " open" : ""}`}
        title={title}
        aria-label={title}
        onClick={() => setOpen((v) => !v)}
      >
        <Icon name={icon} />
        <span class="filter-caret"><Icon name="chevron-down" size={13} /></span>
      </button>

      {open && (
        <div class="filter-pop">
          <input
            ref={inputRef}
            class="input filter-pop-search"
            placeholder={placeholder}
            value={query}
            onInput={(e) => setQuery((e.target as HTMLInputElement).value)}
          />
          <div class="filter-list">
            <button
              class={!active ? "filter-opt active" : "filter-opt"}
              onClick={() => pick(undefined)}
            >
              {allLabel}
            </button>
            {filtered.map((o) => (
              <button
                key={o}
                class={value === o ? "filter-opt active" : "filter-opt"}
                onClick={() => pick(o)}
                title={format ? format(o) : o}
              >
                {format ? format(o) : o}
              </button>
            ))}
            {filtered.length === 0 && <div class="filter-empty">No matches</div>}
          </div>
        </div>
      )}
    </div>
  );
}
