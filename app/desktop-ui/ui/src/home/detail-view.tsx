import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import { type BookmarkRow, type Clip, clipApi, mediaUrl } from "../lib/api.ts";
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
  /** Opens the collection picker for this clip (grid path wires it; collection
   *  detail leaves it unset — clips there are already in a collection). */
  onAddToCollection?: (clip: Clip) => void;
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

// Bookmark palette (matches the engine defaults / Vice's highlight colors).
const BOOKMARK_COLORS = ["#f59e0b", "#e5544b", "#b48ead", "#7aa2f7", "#9ece6a", "#e0af68", "#0db9d7", "#bb9af7"];

// Small colored ticks laid over the timeline / trim track. Clicking one seeks.
function BookmarkMarkers(
  { bookmarks, duration, onSeek }: {
    bookmarks: BookmarkRow[];
    duration: number;
    onSeek: (t: number) => void;
  },
) {
  if (!duration) return null;
  return (
    <>
      {bookmarks.map((bm) => {
        const pct = Math.min(100, Math.max(0, (bm.time_seconds / duration) * 100));
        return (
          <button
            class="bm-marker"
            key={bm.seq}
            title={`${bm.label} — ${formatDuration(bm.time_seconds)}`}
            style={{ left: `${pct}%`, background: bm.color }}
            onClick={() => onSeek(bm.time_seconds)}
          />
        );
      })}
    </>
  );
}

// Inline editor replacing a bookmark row (label + color palette, Save/Cancel).
function BookmarkEditor(
  { bm, busy, onSave, onCancel }: {
    bm: BookmarkRow;
    busy: boolean;
    onSave: (label: string, color: string) => void;
    onCancel: () => void;
  },
) {
  const [label, setLabel] = useState(bm.label);
  const [color, setColor] = useState(bm.color);
  return (
    <li class="bm-row bm-editing">
      <input
        class="input bm-label-input"
        autoFocus
        value={label}
        onInput={(e) => setLabel((e.target as HTMLInputElement).value)}
        onKeyDown={(e) => {
          e.stopPropagation();
          if (e.key === "Enter") onSave(label.trim() || bm.label, color);
          if (e.key === "Escape") onCancel();
        }}
      />
      <span class="bm-palette">
        {BOOKMARK_COLORS.map((c) => (
          <button
            key={c}
            class={`bm-swatch-btn ${color === c ? "active" : ""}`}
            style={{ background: c }}
            title={c}
            onClick={() => setColor(c)}
          />
        ))}
      </span>
      <button class="btn btn-primary" disabled={busy} onClick={() => onSave(label.trim() || bm.label, color)}>
        Save
      </button>
      <button class="btn" disabled={busy} onClick={onCancel}>
        Cancel
      </button>
    </li>
  );
}

export function DetailView(
  { clips, index, allHashtags, onIndex, onClose, onChanged, onClipUpdate, onDelete, onAddToCollection }: Props,
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

  const [bookmarks, setBookmarks] = useState<BookmarkRow[]>([]);
  const [editingSeq, setEditingSeq] = useState<number | null>(null);
  const [trimming, setTrimming] = useState(false);
  const [trim, setTrim] = useState({ start: 0, end: 0 });
  const [trimBusy, setTrimBusy] = useState(false);
  const trackRef = useRef<HTMLDivElement | null>(null);
  const trimStart = trim.start;
  const trimEnd = trim.end;
  // Real video duration when metadata has loaded, else the catalog value.
  const dur = duration || clip?.duration_seconds || 0;

  const multitrack = useMultiTrackAudio(videoEl, clip ? mediaUrl(clip) : null);

  const hasPrev = index > 0;
  const hasNext = index < clips.length - 1;
  const prev = () => hasPrev && onIndex(index - 1);
  const next = () => hasNext && onIndex(index + 1);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (editing || fsMode || editingSeq !== null) return;
      if (trimming) {
        // While trimming, Esc cancels trim mode; arrow keys belong to the
        // focused trim handle, so don't navigate clips with them.
        if (e.key === "Escape") setTrimming(false);
        return;
      }
      if (e.key === "Escape") onClose();
      if (e.key === "ArrowRight") next();
      if (e.key === "ArrowLeft") prev();
    };
    document.addEventListener("keydown", onKey, true);
    return () => document.removeEventListener("keydown", onKey, true);
    // Deps are everything the closure reads; without them the listener was
    // re-subscribed on EVERY render (including ~4 Hz timeupdate-driven ones).
  }, [editing, fsMode, editingSeq, trimming, hasPrev, hasNext, index, onIndex, onClose]);

  useEffect(() => {
    setEditing(false);
    setCurrent(0);
    setDuration(0);
    setActionError(null);
    setFsMode(false);
    setTrimming(false);
    setTrim({ start: 0, end: 0 });
    const v = videoRef.current;
    if (v && clip) {
      v.pause();
      v.src = mediaUrl(clip);
      v.load();
      v.play().catch(() => {});
    }
  }, [clip?.id, clip?.source]);

  // Bookmarks live in the catalog next to the clip; refresh whenever the clip
  // changes (replay clips simply have none).
  useEffect(() => {
    let cancelled = false;
    setBookmarks([]);
    setEditingSeq(null);
    if (clip) {
      clipApi.listBookmarks(clip)
        .then((rows) => { if (!cancelled) setBookmarks(rows); })
        .catch(() => {});
    }
    return () => { cancelled = true; };
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

  function seekTo(t: number) {
    const v = videoRef.current;
    if (v) v.currentTime = t;
    setCurrent(t);
  }

  const refreshBookmarks = async () => {
    try {
      setBookmarks(await clipApi.listBookmarks(clip));
    } catch {
      // Mutations surface their own errors; keep the list as-is here.
    }
  };

  async function addBookmark() {
    if (busy) return;
    setBusy(true);
    const res = await clipApi.addBookmark(
      clip,
      current,
      `Bookmark ${bookmarks.length + 1}`,
      "#f59e0b",
    );
    setBusy(false);
    if (res.ok) {
      setActionError(null);
      await refreshBookmarks();
    } else {
      setActionError(res.error ?? "Couldn't add bookmark");
    }
  }

  async function saveBookmark(bm: BookmarkRow, label: string, color: string) {
    if (busy) return;
    setBusy(true);
    const res = await clipApi.updateBookmark(clip, bm.seq, label, color);
    setBusy(false);
    if (res.ok) {
      setActionError(null);
      setEditingSeq(null);
      await refreshBookmarks();
    } else {
      setActionError(res.error ?? "Couldn't update bookmark");
    }
  }

  async function deleteBookmark(bm: BookmarkRow) {
    if (busy) return;
    setBusy(true);
    const res = await clipApi.deleteBookmark(clip, bm.seq);
    setBusy(false);
    if (res.ok) {
      setActionError(null);
      setEditingSeq(null);
      await refreshBookmarks();
    } else {
      setActionError(res.error ?? "Couldn't delete bookmark");
    }
  }

  function toggleTrim() {
    if (trimming) {
      setTrimming(false);
      return;
    }
    setTrim({ start: 0, end: dur });
    setTrimming(true);
  }

  function startTrimDrag(e: PointerEvent, which: "start" | "end") {
    e.preventDefault();
    const move = (ev: PointerEvent) => {
      const track = trackRef.current;
      if (!track) return;
      const rect = track.getBoundingClientRect();
      const t = Math.min(Math.max((ev.clientX - rect.left) / rect.width, 0), 1) * dur;
      // Keep the selected span at least 0.5s (engine minimum).
      if (which === "start") {
        setTrim((prev) => ({ ...prev, start: Math.min(t, prev.end - 0.5) }));
      } else {
        setTrim((prev) => ({ ...prev, end: Math.max(t, prev.start + 0.5) }));
      }
    };
    const up = () => {
      document.removeEventListener("pointermove", move);
      document.removeEventListener("pointerup", up);
      document.removeEventListener("pointercancel", up);
    };
    document.addEventListener("pointermove", move);
    document.addEventListener("pointerup", up);
    document.addEventListener("pointercancel", up);
  }

  function onTrimKey(e: KeyboardEvent, which: "start" | "end") {
    const delta = e.key === "ArrowLeft" ? -0.1 : e.key === "ArrowRight" ? 0.1 : 0;
    if (!delta) return;
    e.preventDefault();
    if (which === "start") {
      setTrim((prev) => ({
        ...prev,
        start: Math.min(Math.max(prev.start + delta, 0), prev.end - 0.5),
      }));
    } else {
      setTrim((prev) => ({
        ...prev,
        end: Math.max(Math.min(prev.end + delta, dur), prev.start + 0.5),
      }));
    }
  }

  async function applyTrim() {
    if (trimBusy) return;
    setTrimBusy(true);
    const res = await clipApi.trim(clip, trimStart, trimEnd);
    setTrimBusy(false);
    if (res.ok) {
      setActionError(null);
      const newDuration = trimEnd - trimStart;
      onClipUpdate({ ...clip, duration_seconds: newDuration });
      setDuration(newDuration);
      setCurrent(0);
      setTrimming(false);
      // The file on disk was replaced in place, so force the player to
      // re-fetch it (the custom protocol may serve the stale file otherwise).
      const v = videoRef.current;
      if (v) {
        v.pause();
        v.removeAttribute("src");
        v.load();
        v.src = mediaUrl(clip);
        v.load();
      }
      await refreshBookmarks(); // engine retranslates bookmark times
      onChanged();
    } else {
      setActionError(res.error ?? "Couldn't trim clip");
    }
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
            onTimeUpdate={(e) => {
              const v = e.currentTarget;
              const t = v.currentTime;
              setCurrent(t);
              // Trim preview loop: rewind to the selection start while playing.
              if (trimming && !fsMode && t >= trimEnd - 0.02) {
                v.currentTime = trimStart;
              }
            }}
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
                {trimming
                  ? (
                    <div class="trim-track" ref={trackRef}>
                      <BookmarkMarkers bookmarks={bookmarks} duration={dur} onSeek={seekTo} />
                      <div
                        class="trim-playhead"
                        style={{ left: `${(current / dur) * 100}%` }}
                      />
                      <div
                        class="trim-overlay"
                        style={{
                          left: `${(trimStart / dur) * 100}%`,
                          width: `${((trimEnd - trimStart) / dur) * 100}%`,
                        }}
                      />
                      <div
                        class="trim-handle trim-start"
                        role="slider"
                        aria-label="Trim start"
                        aria-valuemin={0}
                        aria-valuemax={dur}
                        aria-valuenow={trimStart}
                        tabIndex={0}
                        style={{ left: `${(trimStart / dur) * 100}%` }}
                        onPointerDown={(e) => startTrimDrag(e, "start")}
                        onKeyDown={(e) => onTrimKey(e, "start")}
                      />
                      <div
                        class="trim-handle trim-end"
                        role="slider"
                        aria-label="Trim end"
                        aria-valuemin={0}
                        aria-valuemax={dur}
                        aria-valuenow={trimEnd}
                        tabIndex={0}
                        style={{ left: `${(trimEnd / dur) * 100}%` }}
                        onPointerDown={(e) => startTrimDrag(e, "end")}
                        onKeyDown={(e) => onTrimKey(e, "end")}
                      />
                    </div>
                  )
                  : (
                    <div class="timeline-wrap">
                      <input
                        class="timeline"
                        type="range"
                        min={0}
                        max={duration || 0}
                        step={0.05}
                        value={current}
                        onInput={onSeek}
                      />
                      <BookmarkMarkers bookmarks={bookmarks} duration={duration} onSeek={seekTo} />
                    </div>
                  )}
                <div class="detail-controls-row">
                  {trimming
                    ? (
                      <span class="trim-readout">
                        <span class="trim-range">
                          {formatDuration(trimStart)} – {formatDuration(trimEnd)}
                        </span>
                        <button class="btn btn-primary" disabled={trimBusy} onClick={applyTrim}>
                          Apply
                        </button>
                        <button class="btn" disabled={trimBusy} onClick={() => setTrimming(false)}>
                          Cancel
                        </button>
                      </span>
                    )
                    : (
                      <span class="time">
                        {formatDuration(current)} / {formatDuration(duration)}
                      </span>
                    )}
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
                    {clip.source === "manual" && (
                      <button
                        class="act-btn"
                        title="Add bookmark at current time"
                        onClick={addBookmark}
                      >
                        <Icon name="bookmark" size={16} />
                      </button>
                    )}
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
                    {onAddToCollection && (
                      <button
                        class="act-btn"
                        title="Add to collection"
                        onClick={() => onAddToCollection(clip)}
                      >
                        <Icon name="album" size={16} />
                      </button>
                    )}
                    <button
                      class={`act-btn act-trim ${trimming ? "active" : ""}`}
                      title="Trim"
                      disabled={!dur}
                      onClick={toggleTrim}
                    >
                      <Icon name="scissors" size={16} />
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

          <div class="detail-bookmarks">
            <div class="detail-bookmarks-head"><span class="k">Bookmarks</span></div>
            {bookmarks.length === 0
              ? (
                <p class="bm-empty">
                  No bookmarks — press Ctrl+Shift+F12 while recording to add one.
                </p>
              )
              : (
                <ul class="bm-list">
                  {bookmarks.map((bm) =>
                    editingSeq === bm.seq
                      ? (
                        <BookmarkEditor
                          key={bm.seq}
                          bm={bm}
                          busy={busy}
                          onSave={(label, color) => saveBookmark(bm, label, color)}
                          onCancel={() => setEditingSeq(null)}
                        />
                      )
                      : (
                        <li class="bm-row" key={bm.seq}>
                          <span class="bm-swatch" style={{ background: bm.color }} />
                          <span class="bm-label" title={bm.label}>{bm.label}</span>
                          <span class="bm-time">{formatDuration(bm.time_seconds)}</span>
                          <button class="act-btn" title="Edit bookmark" onClick={() => setEditingSeq(bm.seq)}>
                            <Icon name="pencil" size={14} />
                          </button>
                          <button class="act-btn" title="Delete bookmark" onClick={() => deleteBookmark(bm)}>
                            <Icon name="x" size={14} />
                          </button>
                        </li>
                      ),
                  )}
                </ul>
              )}
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
