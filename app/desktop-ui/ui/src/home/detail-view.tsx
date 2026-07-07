import { useEffect, useRef, useState } from "preact/hooks";
import { type Clip, clipApi, mediaUrl } from "../lib/api.ts";
import { formatDate, formatDuration, formatSize } from "../lib/format.ts";
import { Icon } from "../shell/icons.tsx";
import { enableAllAudioTracks } from "../lib/player.ts";

interface Props {
  clips: Clip[]; // current filtered list, for prev/next navigation
  index: number; // position of the shown clip within clips
  onIndex: (i: number) => void;
  onClose: () => void;
  onChanged: () => void; // refetch after a mutation
  onOpenHashtags: (clip: Clip) => void;
}

// Centered detail overlay: player with volume + scrubbable timeline, a details
// panel with an inline-editable TITLE (independent of the filename) + hashtags +
// favorite, and ◀ ▶ arrows that move between clips without closing.
export function DetailView(
  { clips, index, onIndex, onClose, onChanged, onOpenHashtags }: Props,
) {
  const clip = clips[index];
  const videoRef = useRef<HTMLVideoElement | null>(null);

  const [editing, setEditing] = useState(false);
  const [titleInput, setTitleInput] = useState("");
  const [busy, setBusy] = useState(false);

  const [current, setCurrent] = useState(0);
  const [duration, setDuration] = useState(0);
  const [volume, setVolume] = useState(1);

  const hasPrev = index > 0;
  const hasNext = index < clips.length - 1;
  const prev = () => hasPrev && onIndex(index - 1);
  const next = () => hasNext && onIndex(index + 1);

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
    setCurrent(0);
    setDuration(0);
  }, [clip?.id, clip?.source]);

  if (!clip) return null;

  async function toggleFavorite() {
    await clipApi.setFavorite(clip, !clip.favorite);
    onChanged();
  }

  function startEdit() {
    setTitleInput(clip.title || "Untitled");
    setEditing(true);
  }

  async function commitTitle() {
    const value = titleInput.trim();
    if (!value || value === clip.title) {
      setEditing(false);
      return;
    }
    setBusy(true);
    const res = await clipApi.setTitle(clip, value);
    setBusy(false);
    if (res.ok) {
      setEditing(false);
      onChanged();
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
        disabled={!hasPrev}
        onMouseDown={(e) => e.stopPropagation()}
        onClick={(e) => {
          e.stopPropagation();
          prev();
        }}
        title="Previous"
      >
        <Icon name="chevron-left" size={22} />
      </button>

      <div class="detail" onMouseDown={(e) => e.stopPropagation()}>
        <div class="detail-player">
          {/* key forces a fresh element per clip so no audio bleeds across. */}
          <video
            key={`${clip.source}:${clip.id}`}
            ref={videoRef}
            src={mediaUrl(clip)}
            autoPlay
            onLoadedMetadata={(e) => {
              const v = e.currentTarget;
              setDuration(v.duration || 0);
              v.volume = volume;
              enableAllAudioTracks(v);
            }}
            onTimeUpdate={(e) => setCurrent(e.currentTarget.currentTime)}
            onClick={(e) => {
              const v = e.currentTarget;
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
                <Icon name="volume-2" size={16} />
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
                    value={titleInput}
                    disabled={busy}
                    onInput={(e) => setTitleInput((e.target as HTMLInputElement).value)}
                    onKeyDown={(e) => {
                      if (e.key === "Enter") commitTitle();
                      if (e.key === "Escape") setEditing(false);
                    }}
                  />
                  <button class="btn btn-primary" disabled={busy} onClick={commitTitle}>
                    Save
                  </button>
                </div>
              )
              : (
                <div class="detail-name" title={clip.title}>
                  <span class="name-text">{clip.title || "Untitled"}</span>
                  <button class="icon-btn small" title="Rename" onClick={startEdit}>
                    <Icon name="pencil" size={15} />
                  </button>
                  <button
                    class={`icon-btn small ${clip.favorite ? "active" : ""}`}
                    title={clip.favorite ? "Remove from favorites" : "Add to favorites"}
                    onClick={toggleFavorite}
                  >
                    <Icon name="star" size={15} filled={clip.favorite} />
                  </button>
                </div>
              )}
          </div>
          <div class="detail-filename" title={clip.video_file}>{clip.video_file}</div>

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
                <Icon name="hash" size={15} />
              </button>
            </div>
            <div class="tag-list">
              {clip.hashtags.length === 0 && <span class="muted">None</span>}
              {clip.hashtags.map((t) => <span class="tag" key={t}>#{t}</span>)}
            </div>
          </div>
        </div>

        <button
          class="detail-close"
          onMouseDown={(e) => e.stopPropagation()}
          onClick={onClose}
          title="Close (Esc)"
        >
          <Icon name="x" size={18} />
        </button>
      </div>

      <button
        class={`detail-nav right ${hasNext ? "" : "disabled"}`}
        disabled={!hasNext}
        onMouseDown={(e) => e.stopPropagation()}
        onClick={(e) => {
          e.stopPropagation();
          next();
        }}
        title="Next"
      >
        <Icon name="chevron-right" size={22} />
      </button>
    </div>
  );
}
