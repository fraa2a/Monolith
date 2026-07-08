import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import { type Clip, clipApi } from "../lib/api.ts";
import { Icon } from "../shell/icons.tsx";

interface Props {
  clip: Clip;
  allHashtags: string[];
  onClose: () => void;
  onChanged: () => void;
}

function normalize(raw: string): string {
  return raw.trim().replace(/^#+/, "").replace(/\s+/g, "-").toLowerCase();
}

export function HashtagDialog({ clip, allHashtags, onClose, onChanged }: Props) {
  const [tags, setTags] = useState<string[]>(clip.hashtags);
  const [input, setInput] = useState("");
  const [busy, setBusy] = useState(false);
  const [open, setOpen] = useState(false);
  const inputRef = useRef<HTMLInputElement | null>(null);

  const suggestions = useMemo(() => {
    const q = normalize(input);
    return allHashtags
      .filter((tag) => !tags.includes(tag))
      .filter((tag) => !q || tag.toLowerCase().includes(q))
      .slice(0, 8);
  }, [allHashtags, input, tags]);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") onClose();
    };
    document.addEventListener("keydown", onKey, true);
    return () => document.removeEventListener("keydown", onKey, true);
  }, [onClose]);

  async function add(raw = input) {
    const tag = normalize(raw);
    if (!tag || tags.includes(tag)) {
      setInput("");
      return;
    }
    setBusy(true);
    const res = await clipApi.addHashtag(clip, tag);
    setBusy(false);
    if (res.ok) {
      setTags((items) => [...items, tag]);
      setInput("");
      setOpen(false);
      onChanged();
    }
  }

  async function remove(tag: string) {
    setBusy(true);
    const res = await clipApi.removeHashtag(clip, tag);
    setBusy(false);
    if (res.ok) {
      setTags((items) => items.filter((item) => item !== tag));
      onChanged();
    }
  }

  return (
    <div class="modal-backdrop" onMouseDown={onClose}>
      <div class="modal" onMouseDown={(e) => e.stopPropagation()}>
        <h3 class="modal-title">Hashtags</h3>
        <div class="tag-editor">
          <div class="tag-list editable" onClick={() => inputRef.current?.focus()}>
            {tags.map((tag) => (
              <span class="tag removable" key={tag}>
                #{tag}
                <button class="tag-x" disabled={busy} title={`Remove #${tag}`} onClick={() => remove(tag)}>
                  <Icon name="x" size={12} />
                </button>
              </span>
            ))}
            <span class="tag-input-shell">
              <span>#</span>
              <input
                ref={inputRef}
                class="tag-input"
                placeholder={tags.length ? "tag" : "add tag"}
                value={input}
                disabled={busy}
                onFocus={() => setOpen(true)}
                onInput={(e) => {
                  setInput((e.target as HTMLInputElement).value);
                  setOpen(true);
                }}
                onKeyDown={(e) => {
                  if (e.key === "Enter") {
                    e.preventDefault();
                    add();
                  }
                  if (e.key === "Escape") setOpen(false);
                }}
              />
            </span>
          </div>
          {open && suggestions.length > 0 && (
            <div class="tag-suggestions">
              {suggestions.map((tag) => (
                <button key={tag} onClick={() => add(tag)}>#{tag}</button>
              ))}
            </div>
          )}
        </div>
        <div class="modal-actions">
          <button class="btn" onClick={onClose}>Close</button>
        </div>
      </div>
    </div>
  );
}