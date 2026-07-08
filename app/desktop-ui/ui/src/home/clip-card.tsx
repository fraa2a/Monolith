import { useEffect, useRef, useState } from "preact/hooks";
import { type Clip, clipApi, fetchGameArtwork, mediaUrl, thumbUrl } from "../lib/api.ts";
import { formatDate, formatDuration, formatSize } from "../lib/format.ts";
import { Icon } from "../shell/icons.tsx";
import { enableAllAudioTracks } from "../lib/player.ts";

interface Props {
  clip: Clip;
  onChanged: (clip: Clip) => void;
  onContextMenu: (e: MouseEvent, clip: Clip) => void;
  onFullscreen: (clip: Clip) => void;
  onOpenDetail: (clip: Clip) => void;
}

const HOVER_DELAY_MS = 1000;

function sameDuration(a: number | null | undefined, b: number) {
  return typeof a === "number" && Number.isFinite(a) && Math.abs(a - b) < 0.1;
}

function drawVideoThumb(video: HTMLVideoElement): string | null {
  const vw = video.videoWidth;
  const vh = video.videoHeight;
  if (!vw || !vh) return null;
  const max = 480;
  const scale = Math.min(1, max / Math.max(vw, vh));
  const canvas = document.createElement("canvas");
  canvas.width = Math.max(2, Math.round(vw * scale));
  canvas.height = Math.max(2, Math.round(vh * scale));
  const ctx = canvas.getContext("2d");
  if (!ctx) return null;
  ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
  return canvas.toDataURL("image/png");
}

export function ClipCard({ clip, onChanged, onContextMenu, onFullscreen, onOpenDetail }: Props) {
  const [preview, setPreview] = useState(false);
  const [ready, setReady] = useState(false);
  const [muted, setMuted] = useState(true);
  const [thumbBroken, setThumbBroken] = useState(false);
  const [thumbBust, setThumbBust] = useState(0);
  const [localThumb, setLocalThumb] = useState<string | null>(clip.thumbnail_file);
  const [displayDuration, setDisplayDuration] = useState<number | null>(clip.duration_seconds);
  const [gameIcon, setGameIcon] = useState<string | null>(clip.game_icon_url ?? null);
  const videoRef = useRef<HTMLVideoElement | null>(null);
  const hoverTimer = useRef<number | undefined>(undefined);
  const regenTried = useRef(false);

  useEffect(() => {
    setLocalThumb(clip.thumbnail_file);
    setDisplayDuration(clip.duration_seconds);
    setThumbBroken(false);
    regenTried.current = false;
  }, [clip.id, clip.source, clip.thumbnail_file, clip.duration_seconds]);

  useEffect(() => {
    let active = true;
    if (clip.game_icon_url) {
      setGameIcon(clip.game_icon_url);
      return;
    }
    if (clip.discord_app_id || clip.game_process_name) {
      fetchGameArtwork(clip).then((art) => {
        if (active) setGameIcon(art.icon ?? null);
      });
    } else {
      setGameIcon(null);
    }
    return () => {
      active = false;
    };
  }, [clip.discord_app_id, clip.game_process_name, clip.game_icon_url]);

  useEffect(() => () => clearTimeout(hoverTimer.current), []);

  useEffect(() => {
    let cancelled = false;
    const needsThumb = !clip.thumbnail_file;
    const video = document.createElement("video");
    video.preload = needsThumb ? "auto" : "metadata";
    video.muted = true;
    video.playsInline = true;

    const finish = () => {
      video.removeAttribute("src");
      video.load();
    };

    video.onloadedmetadata = async () => {
      if (cancelled) return;
      const duration = video.duration;
      if (Number.isFinite(duration) && duration > 0 && !sameDuration(clip.duration_seconds, duration)) {
        setDisplayDuration(duration);
        const res = await clipApi.setDuration(clip, duration);
        if (!cancelled && res.ok) onChanged({ ...clip, duration_seconds: duration });
      }
      if (!needsThumb) finish();
    };

    video.onloadeddata = async () => {
      if (cancelled || !needsThumb) return;
      const dataUrl = drawVideoThumb(video);
      if (!dataUrl) {
        finish();
        return;
      }
      const res = await clipApi.saveCapturedThumb(clip, dataUrl);
      if (!cancelled && res.ok && typeof res.thumbnail_file === "string") {
        setLocalThumb(res.thumbnail_file);
        setThumbBust(Date.now());
        onChanged({ ...clip, thumbnail_file: res.thumbnail_file });
      }
      finish();
    };

    video.onerror = finish;
    video.src = mediaUrl(clip);
    video.load();

    return () => {
      cancelled = true;
      finish();
    };
  }, [clip.id, clip.source, clip.video_file]);

  const thumb = localThumb ? thumbUrl({ ...clip, thumbnail_file: localThumb }) : null;
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

  async function onThumbError() {
    if (regenTried.current) {
      setThumbBroken(true);
      return;
    }
    regenTried.current = true;
    const res = await clipApi.regenThumb(clip);
    if (res.ok) {
      setThumbBust(Date.now());
      onChanged(clip);
    } else {
      setThumbBroken(true);
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
        {displayDuration
          ? <div class="dur-badge">{formatDuration(displayDuration)}</div>
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
          <span class="dot">·</span>
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
