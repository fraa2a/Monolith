import { useEffect, useRef, useState } from "preact/hooks";
import { type Clip, clipApi, exeIconUrl, mediaUrl, thumbUrl } from "../lib/api.ts";
import { appLabel, formatDate, formatDuration, formatSize } from "../lib/format.ts";
import { Icon } from "../shell/icons.tsx";
import { useMultiTrackAudio } from "../lib/multitrack.ts";

interface Props {
  clip: Clip;
  onChanged: (clip: Clip) => void;
  onContextMenu: (e: MouseEvent, clip: Clip) => void;
  onFullscreen: (clip: Clip, initialTime: number) => void;
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
  const [exeIcon, setExeIcon] = useState<string | null>(null);
  const [videoEl, setVideoEl] = useState<HTMLVideoElement | null>(null);
  const hoverTimer = useRef<number | undefined>(undefined);
  const regenTried = useRef(false);

  const multitrack = useMultiTrackAudio(preview ? videoEl : null, preview ? mediaUrl(clip) : null);

  useEffect(() => {
    setLocalThumb(clip.thumbnail_file);
    setDisplayDuration(clip.duration_seconds);
    setThumbBroken(false);
    regenTried.current = false;
  }, [clip.id, clip.source, clip.thumbnail_file, clip.duration_seconds]);

  // Icon is always resolved locally: prefer the icon embedded in the game's
  // own executable (no network, no cache dependency), then fall back to
  // whatever list_clips already resolved from the local Discord artwork
  // cache (game_icon_url) — never fetched here, so opening the library never
  // triggers a network call. A game newly added to the cache picks up its
  // icon on the next scheduled refresh (see game_catalog::refresh_stale).
  useEffect(() => {
    let active = true;
    if (!clip.game_executable_path) {
      setExeIcon(null);
      return;
    }
    exeIconUrl(clip.game_executable_path, clip.game_process_name ?? "").then((dataUrl) => {
      if (active) setExeIcon(dataUrl);
    });
    return () => {
      active = false;
    };
  }, [clip.game_executable_path, clip.game_process_name]);

  const gameIcon = exeIcon ?? clip.game_icon_url ?? null;
  const hasGame = !!(clip.game_process_name || clip.game_display_name);

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
    const v = videoEl;
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

  async function toggleFavorite(e: MouseEvent) {
    e.stopPropagation();
    const res = await clipApi.setFavorite(clip, !clip.favorite);
    if (res.ok) onChanged({ ...clip, favorite: !clip.favorite });
  }

  const tagCount = clip.hashtags.length;
  const singleTag = tagCount === 1 ? clip.hashtags[0] : null;

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
            ref={setVideoEl}
            class={`card-video ${ready ? "ready" : ""}`}
            src={mediaUrl(clip)}
            muted={muted}
            loop
            playsInline
            onLoadedData={(e) => {
              setReady(true);
              e.currentTarget.play().catch(() => {});
            }}
          />
        )}

        <button
          class={`fav-toggle ${clip.favorite ? "on" : ""}`}
          title={clip.favorite ? "Remove from favorites" : "Add to favorites"}
          onClick={toggleFavorite}
        >
          <Icon name="star" size={14} filled={clip.favorite} />
        </button>

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
                setMuted((m) => {
                  const next = !m;
                  multitrack.setMuted(next);
                  return next;
                });
              }}
            >
              <Icon name={muted ? "volume-x" : "volume-2"} size={16} />
            </button>
            <button
              class="ov-btn"
              title="Fullscreen"
              onClick={(e) => {
                e.stopPropagation();
                onFullscreen(clip, videoEl?.currentTime ?? 0);
              }}
            >
              <Icon name="maximize" size={16} />
            </button>
          </div>
        )}
      </div>

      <div class="card-meta" onClick={() => onOpenDetail(clip)}>
        <div class="card-name-row">
          <div class="card-name" title={clip.title}>{clip.title || "Untitled"}</div>
          {tagCount > 0 && (
            <span class="tag blob" title={tagCount > 1 ? clip.hashtags.map((t) => `#${t}`).join(", ") : `#${singleTag}`}>
              {tagCount > 1 ? "…" : `#${singleTag}`}
            </span>
          )}
        </div>
        <div class="card-sub">
          {hasGame
            ? (gameIcon
              ? <img class="game-icon" src={gameIcon} alt="" title={appLabel(clip.game_display_name, clip.game_process_name)} />
              : (
                <span class="game-icon placeholder" title={appLabel(clip.game_display_name, clip.game_process_name)}>
                  <Icon name="gamepad" size={13} />
                </span>
              ))
            : clip.source === "manual"
            ? (
              <span class="game-icon placeholder" title="Screen Recording">
                <Icon name="monitor" size={13} />
              </span>
            )
            : (
              <span class="game-icon placeholder" title="None">
                <Icon name="circle-slash" size={13} />
              </span>
            )}
          <span class="dot">·</span>
          <span>{formatDate(clip.created_at_utc)}</span>
          <span class="dot">·</span>
          <span>{formatSize(clip.size_bytes)}</span>
        </div>
      </div>
    </div>
  );
}
