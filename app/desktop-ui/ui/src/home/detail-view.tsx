import { useEffect, useRef, useState } from "preact/hooks";
import { type Clip, clipApi, mediaUrl } from "../lib/api.ts";
import { formatDate, formatDuration, formatSize } from "../lib/format.ts";

interface Props {
  clips: Clip[];      // current filtered list, for prev/next navigation
  index: number;      // position of the shown clip within clips
  onIndex: (i: number) => void;
  onClose: () => void;
  onChanged: () => void; // refetch after rename/hashtag mutation
  onOpenHashtags: (clip: Clip) => void;
}

// Centered detail overlay (NOT fullscreen, deno.md §2.2): player with volume +
// scrubbable timeline, details panel with inline-editable name + hashtags, and
// ◀ ▶ arrows to move between clips without closing.
export function DetailView(
  { clips, index, onIndex, onClose, onChanged, onOpenHashtags }: Props,
) {
  const clip = clips[index];
  const videoRef = useRef<HTMLVideoElement | null>(null);

  const [editing, setEditing] = useState(false);
  const [nameInput, setNameInput] = useState("");
  const [renameErr, setRenameErr] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  // Playback state driven by the <video> element.
  const [current, setCurrent] = useState(0);
  const [duration, setDuration] = useState(0);
  const [volume, setVolume] = useState(1);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (editing) return;
      if (e.key === "Escape") onClose();
      if (e.key === "ArrowRight") next();
      if (e.key === "ArrowLeft") prev();
    };
    document.addEventListener("keydown", onKey, true);
    return () => document.removeEventListener("keydown", onKey, true);
  });

  // Reset transient state when the shown clip changes.
  useEffect(() => {
    setEditing(false);
    setRenameErr(null);
    setCurrent(0);
    setDuration(0);
  }, [clip?.id, clip?.source]);

  if (!clip) return null;

  const hasPrev = index > 0;
  const hasNext = index < clips.length - 1;
  const prev = () => hasPrev && onIndex(index - 1);
  const next = () => hasNext && onIndex(index + 1);

  const stem = clip.video_file.replace(/\.[^.]+$/, "");
  const ext = clip.video_file.slice(stem.length);

  function startEdit() {
    setNameInput(stem);
    setRenameErr(null);
    setEditing(true);
  }

  async function commitRename() {
    const next = nameInput.trim();
    if (!next || next === stem) {
      setEditing(false);
      return;
    }
    setBusy(true);
    const res = await clipApi.rename(clip, next);
    setBusy(false);
    if (res.ok) {
      setEditing(false);
      onChanged();
    } else {
      setRenameErr(res.error ?? "rename failed");
    }
  }

  function onSeek(e: Event) {
    const t = Number((e.target as HTMLInputElement).value);
    const v = videoRef.current;
    if (v) v.currentTime = t;
    setCurrent(t);
  }

  function onVolume(e: Event) {
    const val = Number((e.target as HTMLInputElement).value);
    setVolume(val);
    if (videoRef.current) videoRef.current.volume = val;
  }

  return (
    <div class="detail-backdrop" onMouseDown={onClose}>
      <button
        class={`detail-nav left ${hasPrev ? "" : "disabled"}`}
        onClick={(e) => {
          e.stopPropagation();
          prev();
        }}
        title="Previous"
      >
        ◀
      </button>

      <div class="detail" onMouseDown={(e) => e.stopPropagation()}>
        <div class="detail-player">
          <video
            ref={videoRef}
            src={mediaUrl(clip)}
            autoPlay
            onLoadedMetadata={(e) => {
              const v = e.target as HTMLVideoElement;
              setDuration(v.duration || 0);
              v.volume = volume;
            }}
            onTimeUpdate={(e) => setCurrent((e.target as HTMLVideoElement).currentTime)}
            onClick={(e) => {
              const v = e.target as HTMLVideoElement;
              v.paused ? v.play() : v.pause();
            }}
          />
          <div class="detail-controls">
            <input
              class="timeline"
              type="range"
              min={0}
              max={duration || 0}
              step={0.05}
              value={current}
              onInput={onSeek}
            />
            <div class="detail-controls-row">
              <span class="time">
                {formatDuration(current)} / {formatDuration(duration)}
              </span>
              <div class="vol">
                <span>🔊</span>
                <input
                  class="vol-slider"
                  type="range"
                  min={0}
                  max={1}
                  step={0.01}
                  value={volume}
                  onInput={onVolume}
                />
              </div>
            </div>
          </div>
        </div>

        <div class="detail-panel">
          <div class="detail-name-row">
            {editing
              ? (
                <div class="rename">
                  <input
                    class="input"
                    autoFocus
                    value={nameInput}
                    disabled={busy}
                    onInput={(e) => setNameInput((e.target as HTMLInputElement).value)}
                    onKeyDown={(e) => {
                      if (e.key === "Enter") commitRename();
                      if (e.key === "Escape") setEditing(false);
                    }}
                  />
                  <span class="ext">{ext}</span>
                  <button class="btn btn-primary" disabled={busy} onClick={commitRename}>
                    Save
                  </button>
                </div>
              )
              : (
                <div class="detail-name" title={clip.video_file}>
                  <span class="name-text">{clip.video_file}</span>
                  <button class="icon-btn small" title="Rename" onClick={startEdit}>✎</button>
                </div>
              )}
          </div>
          {renameErr && <div class="err">{renameErr}</div>}

          <div class="detail-meta">
            <div>
              <span class="k">Game</span>
              <span class="v">
                {clip.game_display_name ?? clip.game_process_name ?? "Unknown"}
              </span>
            </div>
            <div>
              <span class="k">Size</span>
              <span class="v">{formatSize(clip.size_bytes)}</span>
            </div>
            <div>
              <span class="k">Saved</span>
              <span class="v">{formatDate(clip.created_at_utc)}</span>
            </div>
            <div>
              <span class="k">Source</span>
              <span class="v">{clip.source}</span>
            </div>
          </div>

          <div class="detail-tags">
            <div class="detail-tags-head">
              <span class="k">Hashtags</span>
              <button class="icon-btn small" title="Edit hashtags" onClick={() => onOpenHashtags(clip)}>
                #
              </button>
            </div>
            <div class="tag-list">
              {clip.hashtags.length === 0 && <span class="muted">None</span>}
              {clip.hashtags.map((t) => <span class="tag" key={t}>#{t}</span>)}
            </div>
          </div>
        </div>

        <button class="detail-close" onClick={onClose} title="Close (Esc)">×</button>
      </div>

      <button
        class={`detail-nav right ${hasNext ? "" : "disabled"}`}
        onClick={(e) => {
          e.stopPropagation();
          next();
        }}
        title="Next"
      >
        ▶
      </button>
    </div>
  );
}
