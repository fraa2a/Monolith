import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import { type Clip, clipApi, mediaUrl } from "../lib/api.ts";
import { appLabel, formatDate, formatDuration, formatSize } from "../lib/format.ts";
import { Icon } from "../shell/icons.tsx";
import { useMultiTrackAudio } from "../lib/multitrack.ts";
import { appWindow } from "../lib/window.ts";
import { FullscreenChrome } from "./fullscreen.tsx";

interface Props {
  clips: Clip[];
  index: number;
  allHashtags: string[];
  onIndex: (i: number) => void;
  onClose: () => void;
  onChanged: () => void;
  onClipUpdate: (clip: Clip) => void;
  onDelete: (clip: Clip) => void;
}

function normalizeTag(value: string): string {
  return value.trim().replace(/^#+/, "").replace(/\s+/g, "-").toLowerCase();
}

function TagEditor(
  { clip, allHashtags, onClipUpdate, onChanged }: {
    clip: Clip;
    allHashtags: string[];
    onClipUpdate: (clip: Clip) => void;
    onChanged: () => void;
  },
) {
  const [input, setInput] = useState("");
  const [open, setOpen] = useState(false);
  const [busy, setBusy] = useState(false);
  const wrapRef = useRef<HTMLDivElement | null>(null);

  const suggestions = useMemo(() => {
    const q = normalizeTag(input);
    return allHashtags
      .filter((tag) => !clip.hashtags.includes(tag))
      .filter((tag) => !q || tag.toLowerCase().includes(q))
      .slice(0, 7);
  }, [allHashtags, clip.hashtags, input]);

  useEffect(() => {
    const onDown = (e: MouseEvent) => {
      if (!wrapRef.current?.contains(e.target as Node)) setOpen(false);
    };
    document.addEventListener("mousedown", onDown, true);
    return () => document.removeEventListener("mousedown", onDown, true);
  }, []);

  const addTag = async (raw: string) => {
    const tag = normalizeTag(raw);
    if (!tag || clip.hashtags.includes(tag) || busy) return;
    setBusy(true);
    const res = await clipApi.addHashtag(clip, tag);
    setBusy(false);
    if (res.ok) {
      onClipUpdate({ ...clip, hashtags: [...clip.hashtags, tag] });
      setInput("");
      setOpen(false);
      onChanged();
    }
  };

  const removeTag = async (tag: string) => {
    if (busy) return;
    setBusy(true);
    const res = await clipApi.removeHashtag(clip, tag);
    setBusy(false);
    if (res.ok) {
      onClipUpdate({ ...clip, hashtags: clip.hashtags.filter((item) => item !== tag) });
      onChanged();
    }
  };

  return (
    <div class="tag-editor" ref={wrapRef}>
      <div class="tag-list editable">
        {clip.hashtags.map((tag) => (
          <span class="tag removable" key={tag}>
            #{tag}
            <button class="tag-x" title={`Remove #${tag}`} onClick={() => removeTag(tag)}>
              <Icon name="x" size={11} />
            </button>
          </span>
        ))}
        <span class="tag-input-shell">
          <span>#</span>
          <input
            class="tag-input"
            value={input}
            disabled={busy}
            placeholder={clip.hashtags.length ? "tag" : "add tag"}
            onFocus={() => setOpen(true)}
            onInput={(e) => {
              setInput((e.target as HTMLInputElement).value);
              setOpen(true);
            }}
            onKeyDown={(e) => {
              e.stopPropagation();
              if (e.key === "Enter") {
                e.preventDefault();
                addTag(input);
              }
              if (e.key === "Escape") setOpen(false);
            }}
          />
        </span>
      </div>
      {open && suggestions.length > 0 && (
        <div class="tag-suggestions">
          {suggestions.map((tag) => (
            <button key={tag} onClick={() => addTag(tag)}>#{tag}</button>
          ))}
        </div>
      )}
    </div>
  );
}

export function DetailView(
  { clips, index, allHashtags, onIndex, onClose, onChanged, onClipUpdate, onDelete }: Props,
) {
  const clip = clips[index];
  const videoRef = useRef<HTMLVideoElement | null>(null);
  const [videoEl, setVideoEl] = useState<HTMLVideoElement | null>(null);

  const [editing, setEditing] = useState(false);
  const [titleInput, setTitleInput] = useState("");
  const [busy, setBusy] = useState(false);

  const [current, setCurrent] = useState(0);
  const [duration, setDuration] = useState(0);
  const [volume, setVolume] = useState(1);
  const [actionError, setActionError] = useState<string | null>(null);
  const [fsMode, setFsMode] = useState(false);

  const multitrack = useMultiTrackAudio(videoEl, clip ? mediaUrl(clip) : null);

  const hasPrev = index > 0;
  const hasNext = index < clips.length - 1;
  const prev = () => hasPrev && onIndex(index - 1);
  const next = () => hasNext && onIndex(index + 1);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (editing || fsMode) return;
      if (e.key === "Escape") onClose();
      if (e.key === "ArrowRight") next();
      if (e.key === "ArrowLeft") prev();
    };
    document.addEventListener("keydown", onKey, true);
    return () => document.removeEventListener("keydown", onKey, true);
  });

  useEffect(() => {
    setEditing(false);
    setCurrent(0);
    setDuration(0);
    setActionError(null);
    setFsMode(false);
    const v = videoRef.current;
    if (v && clip) {
      v.pause();
      v.src = mediaUrl(clip);
      v.load();
      v.play().catch(() => {});
    }
  }, [clip?.id, clip?.source]);

  // Fullscreen reuses this same <video> element in place (see render below) —
  // only the surrounding chrome/layout changes, so playback never restarts.
  useEffect(() => {
    if (!fsMode) return;
    appWindow.maximize();
    return () => { appWindow.unmaximize(); };
  }, [fsMode]);

  if (!clip) return null;

  async function toggleFavorite() {
    const nextClip = { ...clip, favorite: !clip.favorite };
    onClipUpdate(nextClip);
    const res = await clipApi.setFavorite(clip, nextClip.favorite);
    if (!res.ok) {
      onClipUpdate(clip);
      setActionError(res.error ?? "Couldn't update favorite");
    } else {
      setActionError(null);
      onChanged();
    }
  }

  function startEdit() {
    setTitleInput(clip.title || "Untitled");
    setEditing(true);
  }

  async function commitTitle() {
    const value = titleInput.trim() || "Untitled";
    if (value === (clip.title || "Untitled")) {
      setEditing(false);
      return;
    }
    setBusy(true);
    const res = await clipApi.setTitle(clip, value);
    setBusy(false);
    if (res.ok) {
      onClipUpdate({ ...clip, title: value });
      setEditing(false);
      setActionError(null);
      onChanged();
    } else {
      setActionError(res.error ?? "Couldn't rename clip");
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
    multitrack.setVolume(val);
  }

  return (
    <div class={`detail-backdrop ${fsMode ? "fs-active" : ""}`} onMouseDown={onClose}>
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
        <Icon name="chevron-left" size={24} />
      </button>

      <div class="detail" onMouseDown={(e) => e.stopPropagation()}>
        <div class="detail-player">
          <video
            ref={(el) => { videoRef.current = el; setVideoEl(el); }}
            src={mediaUrl(clip)}
            autoPlay
            onLoadedMetadata={(e) => {
              const v = e.currentTarget;
              setDuration(v.duration || 0);
              v.volume = volume;
            }}
            onTimeUpdate={(e) => setCurrent(e.currentTarget.currentTime)}
            onClick={(e) => {
              const v = e.currentTarget;
              v.paused ? v.play() : v.pause();
            }}
          />
          {fsMode
            ? (
              <FullscreenChrome
                videoEl={videoEl}
                multitrack={multitrack}
                onClose={() => setFsMode(false)}
              />
            )
            : (
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
                    <button
                      class="act-btn"
                      title="Fullscreen"
                      onClick={() => setFsMode(true)}
                    >
                      <Icon name="maximize" size={16} />
                    </button>
                  </div>
                </div>
              </div>
            )}
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
                      e.stopPropagation();
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
                <div class="detail-name" title="Open containing folder">
                  <span
                    class="name-text"
                    onClick={() => clipApi.revealInExplorer(clip)}
                  >
                    {clip.title || "Untitled"}
                  </span>
                  <div class="detail-name-actions">
                    <button class="act-btn" title="Rename" onClick={startEdit}>
                      <Icon name="pencil" size={16} />
                    </button>
                    <button
                      class={`act-btn act-fav ${clip.favorite ? "active" : ""}`}
                      title={clip.favorite ? "Remove from favorites" : "Add to favorites"}
                      onClick={toggleFavorite}
                    >
                      <Icon name="star" size={16} filled={clip.favorite} />
                    </button>
                  </div>
                </div>
              )}
          </div>
          {actionError && <div class="err">{actionError}</div>}
          <div class="detail-filename" title={clip.video_file}>{clip.video_file}</div>

          <div class="detail-meta">
            <div>
              <span class="k">Game</span>
              <span class="v">
                {appLabel(clip.game_display_name, clip.game_process_name)}
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
              <span class="v">{clip.source === "replay" ? "Saved replay" : "Manual recording"}</span>
            </div>
          </div>

          <div class="detail-tags">
            <div class="detail-tags-head"><span class="k">Hashtags</span></div>
            <TagEditor
              clip={clip}
              allHashtags={allHashtags}
              onClipUpdate={onClipUpdate}
              onChanged={onChanged}
            />
          </div>

          <button class="delete-clip-btn" onClick={() => onDelete(clip)}>
            <Icon name="trash-2" size={16} />
            Delete Clip
          </button>
        </div>
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
        <Icon name="chevron-right" size={24} />
      </button>
    </div>
  );
}
