import { useEffect, useState } from "preact/hooks";
import { type Clip, clipApi } from "../lib/api.ts";

interface Props {
  clip: Clip;
  onClose: () => void;
  onChanged: () => void; // refetch clips after a tag add/remove
}

// Custom popup to add/remove hashtags on a clip. Also reachable from the detail
// view (F4). Tags feed the Home hashtag filter (deno.md extra #1).
export function HashtagDialog({ clip, onClose, onChanged }: Props) {
  const [tags, setTags] = useState<string[]>(clip.hashtags);
  const [input, setInput] = useState("");
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") onClose();
    };
    document.addEventListener("keydown", onKey, true);
    return () => document.removeEventListener("keydown", onKey, true);
  }, [onClose]);

  function normalize(raw: string): string {
    return raw.trim().replace(/^#+/, "").replace(/\s+/g, "-").toLowerCase();
  }

  async function add() {
    const tag = normalize(input);
    if (!tag || tags.includes(tag)) {
      setInput("");
      return;
    }
    setBusy(true);
    const res = await clipApi.addHashtag(clip, tag);
    setBusy(false);
    if (res.ok) {
      setTags((t) => [...t, tag]);
      setInput("");
      onChanged();
    }
  }

  async function remove(tag: string) {
    setBusy(true);
    const res = await clipApi.removeHashtag(clip, tag);
    setBusy(false);
    if (res.ok) {
      setTags((t) => t.filter((x) => x !== tag));
      onChanged();
    }
  }

  return (
    <div class="modal-backdrop" onMouseDown={onClose}>
      <div class="modal" onMouseDown={(e) => e.stopPropagation()}>
        <h3 class="modal-title">Hashtags</h3>
        <div class="tag-list">
          {tags.length === 0 && <span class="muted">No hashtags yet.</span>}
          {tags.map((t) => (
            <span class="tag removable" key={t}>
              #{t}
              <button class="tag-x" disabled={busy} onClick={() => remove(t)}>×</button>
            </span>
          ))}
        </div>
        <div class="tag-add">
          <input
            class="input"
            placeholder="add a hashtag…"
            value={input}
            disabled={busy}
            onInput={(e) => setInput((e.target as HTMLInputElement).value)}
            onKeyDown={(e) => {
              if (e.key === "Enter") add();
            }}
          />
          <button class="btn btn-primary" disabled={busy} onClick={add}>Add</button>
        </div>
        <div class="modal-actions">
          <button class="btn" onClick={onClose}>Close</button>
        </div>
      </div>
    </div>
  );
}
