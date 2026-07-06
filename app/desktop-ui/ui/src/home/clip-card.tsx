import { useEffect, useRef, useState } from "preact/hooks";
import { type Clip, fetchGameIcon, mediaUrl, thumbUrl } from "../lib/api.ts";
import { formatDate, formatDuration, formatSize } from "../lib/format.ts";

interface Props {
  clip: Clip;
  onContextMenu: (e: MouseEvent, clip: Clip) => void;
  onFullscreen: (clip: Clip) => void;
  onOpenDetail: (clip: Clip) => void;
}

// A rounded clip card: mini-player on top (static PNG at rest, hover preview
// with audio + mute/fullscreen overlay), metadata below. Missing/broken
// thumbnails fall back to a placeholder, and the mini-player still starts on
// hover (deno.md §2.1 + user self-heal requirement #2, UI side).
export function ClipCard({ clip, onContextMenu, onFullscreen, onOpenDetail }: Props) {
  const [hover, setHover] = useState(false);
  const [muted, setMuted] = useState(false);
  const [thumbBroken, setThumbBroken] = useState(false);
  const [gameIcon, setGameIcon] = useState<string | null>(null);
  const videoRef = useRef<HTMLVideoElement | null>(null);

  // Lazily resolve the game icon (Discord CDN) once per card.
  useEffect(() => {
    let active = true;
    if (clip.game_process_name) {
      fetchGameIcon(clip.game_process_name).then((url) => {
        if (active) setGameIcon(url);
      });
    }
    return () => {
      active = false;
    };
  }, [clip.game_process_name]);

  const thumb = thumbUrl(clip);
  const showPlaceholder = !thumb || thumbBroken;

  function enter() {
    setHover(true);
    // Attempt to play the lightweight preview; autoplay-with-audio may be
    // rejected — that's fine, the user can click unmute/play.
    queueMicrotask(() => videoRef.current?.play().catch(() => {}));
  }
  function leave() {
    setHover(false);
    const v = videoRef.current;
    if (v) {
      v.pause();
      v.currentTime = 0;
    }
  }

  return (
    <div
      class="card"
      onContextMenu={(e) => {
        e.preventDefault();
        onContextMenu(e as unknown as MouseEvent, clip);
      }}
      onMouseEnter={enter}
      onMouseLeave={leave}
    >
      <div class="card-media" onClick={() => onOpenDetail(clip)}>
        {hover
          ? (
            <video
              ref={videoRef}
              class="card-video"
              src={mediaUrl(clip)}
              muted={muted}
              loop
              playsInline
            />
          )
          : showPlaceholder
          ? (
            <div class="thumb-placeholder" title="Thumbnail unavailable">
              <svg viewBox="0 0 24 24" width="34" height="34" aria-hidden="true">
                <path
                  fill="currentColor"
                  d="M4 5h16a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1zm1 2v8l4-4 3 3 3-3 4 4V7H5z"
                />
              </svg>
            </div>
          )
          : <img class="card-thumb" src={thumb!} onError={() => setThumbBroken(true)} />}

        {clip.favorite && <div class="fav-badge" title="Favorite">★</div>}
        {clip.duration_seconds
          ? <div class="dur-badge">{formatDuration(clip.duration_seconds)}</div>
          : null}

        {hover && (
          <div class="card-overlay">
            <button
              class="ov-btn"
              title={muted ? "Unmute" : "Mute"}
              onClick={(e) => {
                e.stopPropagation();
                setMuted((m) => !m);
              }}
            >
              {muted ? "🔇" : "🔊"}
            </button>
            <button
              class="ov-btn"
              title="Fullscreen"
              onClick={(e) => {
                e.stopPropagation();
                onFullscreen(clip);
              }}
            >
              ⛶
            </button>
          </div>
        )}
      </div>

      <div class="card-meta">
        <div class="card-name" title={clip.video_file}>{clip.video_file}</div>
        <div class="card-sub">
          {gameIcon
            ? <img class="game-icon" src={gameIcon} alt="" />
            : <span class="game-icon placeholder">🎮</span>}
          <span class="game">
            {clip.game_display_name ?? clip.game_process_name ?? "Unknown"}
          </span>
        </div>
        <div class="card-sub2">
          <span>{formatSize(clip.size_bytes)}</span>
          <span class="dot">•</span>
          <span>{formatDate(clip.created_at_utc)}</span>
        </div>
        {clip.hashtags.length > 0 && (
          <div class="card-tags">
            {clip.hashtags.map((t) => <span class="tag" key={t}>#{t}</span>)}
          </div>
        )}
      </div>
    </div>
  );
}
