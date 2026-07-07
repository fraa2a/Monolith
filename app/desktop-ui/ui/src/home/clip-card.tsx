import { useEffect, useRef, useState } from "preact/hooks";
import { type Clip, clipApi, fetchGameIcon, mediaUrl, thumbUrl } from "../lib/api.ts";
import { formatDate, formatDuration, formatSize } from "../lib/format.ts";
import { Icon } from "../shell/icons.tsx";
import { enableAllAudioTracks } from "../lib/player.ts";

interface Props {
  clip: Clip;
  onContextMenu: (e: MouseEvent, clip: Clip) => void;
  onFullscreen: (clip: Clip) => void;
  onOpenDetail: (clip: Clip) => void;
}

// Delay before a hover preview starts, so brushing across the grid doesn't fire
// dozens of videos and cause the black-flash the user reported.
const HOVER_DELAY_MS = 1000;

export function ClipCard({ clip, onContextMenu, onFullscreen, onOpenDetail }: Props) {
  const [preview, setPreview] = useState(false); // video mounted
  const [ready, setReady] = useState(false); // first frame decoded → fade in
  const [muted, setMuted] = useState(true); // previews always start muted
  const [thumbBroken, setThumbBroken] = useState(false);
  const [thumbBust, setThumbBust] = useState(0); // cache-buster after regen
  const [gameIcon, setGameIcon] = useState<string | null>(null);
  const videoRef = useRef<HTMLVideoElement | null>(null);
  const hoverTimer = useRef<number | undefined>(undefined);
  const regenTried = useRef(false);

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

  // Clean up any pending hover timer on unmount.
  useEffect(() => () => clearTimeout(hoverTimer.current), []);

  const thumb = thumbUrl(clip);
  const showPlaceholder = !thumb || thumbBroken;
  const thumbSrc = thumb ? `${thumb}${thumbBust ? `?v=${thumbBust}` : ""}` : null;

  function enter() {
    clearTimeout(hoverTimer.current);
    hoverTimer.current = setTimeout(() => {
      setPreview(true);
      setReady(false);
      setMuted(true);
    }, HOVER_DELAY_MS) as unknown as number;
  }

  function leave() {
    clearTimeout(hoverTimer.current);
    const v = videoRef.current;
    if (v) {
      // Fully reset audio/playback so nothing bleeds into the next preview.
      v.pause();
      v.muted = true;
      try {
        v.currentTime = 0;
      } catch { /* not seekable yet */ }
    }
    setPreview(false);
    setReady(false);
    setMuted(true);
  }

  // On the thumbnail failing to load, ask the engine to regenerate it once, then
  // retry with a cache-busting query. If that fails too, show the placeholder.
  async function onThumbError() {
    if (regenTried.current) {
      setThumbBroken(true);
      return;
    }
    regenTried.current = true;
    const res = await clipApi.regenThumb(clip);
    if (res.ok) setThumbBust(Date.now());
    else setThumbBroken(true);
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
        {/* Poster stays mounted underneath the preview to avoid a black flash. */}
        {showPlaceholder
          ? (
            <div class="thumb-placeholder" title="Thumbnail unavailable">
              <Icon name="film" size={34} />
            </div>
          )
          : <img class="card-thumb" src={thumbSrc!} onError={onThumbError} />}

        {preview && (
          <video
            ref={videoRef}
            class={`card-video ${ready ? "ready" : ""}`}
            src={mediaUrl(clip)}
            muted={muted}
            loop
            playsInline
            onLoadedMetadata={(e) => enableAllAudioTracks(e.currentTarget)}
            onLoadedData={(e) => {
              setReady(true);
              e.currentTarget.play().catch(() => {});
            }}
          />
        )}

        {clip.favorite && (
          <div class="fav-badge" title="Favorite">
            <Icon name="star" size={14} filled />
          </div>
        )}
        {clip.duration_seconds
          ? <div class="dur-badge">{formatDuration(clip.duration_seconds)}</div>
          : null}

        {preview && (
          <div class="card-overlay">
            <button
              class="ov-btn"
              title={muted ? "Unmute" : "Mute"}
              onClick={(e) => {
                e.stopPropagation();
                setMuted((m) => !m);
              }}
            >
              <Icon name={muted ? "volume-x" : "volume-2"} size={16} />
            </button>
            <button
              class="ov-btn"
              title="Fullscreen"
              onClick={(e) => {
                e.stopPropagation();
                onFullscreen(clip);
              }}
            >
              <Icon name="maximize" size={16} />
            </button>
          </div>
        )}
      </div>

      <div class="card-meta">
        <div class="card-name" title={clip.title}>{clip.title || "Untitled"}</div>
        <div class="card-sub">
          {gameIcon
            ? <img class="game-icon" src={gameIcon} alt="" />
            : <span class="game-icon placeholder"><Icon name="gamepad" size={13} /></span>}
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
