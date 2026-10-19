import { memo } from "preact/compat";
import { useEffect, useLayoutEffect, useRef, useState } from "preact/hooks";
import { type Clip, clipApi, clipKey, exeIconUrl, mediaUrl, thumbUrl } from "../lib/api.ts";
import { enqueueMediaProbe } from "../lib/media-probes.ts";
import { appLabel, formatDate, formatDuration, formatSize } from "../lib/format.ts";
import { Icon } from "../shell/icons.tsx";
import { useMultiTrackAudio } from "../lib/multitrack.ts";
import { Button } from "../components/ui/button.tsx";

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

export const ClipCard = memo(function ClipCard(
  { clip, onChanged, onContextMenu, onFullscreen, onOpenDetail }: Props,
) {
  const [preview, setPreview] = useState(false);
  const [ready, setReady] = useState(false);
  const [muted, setMuted] = useState(true);
  const [thumbBroken, setThumbBroken] = useState(false);
  const [thumbBust, setThumbBust] = useState(0);
  const [visible, setVisible] = useState(false);
  const cardRef = useRef<HTMLDivElement>(null);
  const latest = useRef({ clip, onChanged });
  latest.current = { clip, onChanged };
  const [displayDuration, setDisplayDuration] = useState<number | null>(clip.duration_seconds);
  const [exeIcon, setExeIcon] = useState<string | null>(null);
  const [videoEl, setVideoEl] = useState<HTMLVideoElement | null>(null);
  const hoverTimer = useRef<number | undefined>(undefined);
  const regenTried = useRef(false);
  const thumbAttempts = useRef(0);
  const thumbRetryTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const thumbRepair = useRef<(() => void) | undefined>(undefined);
  const mounted = useRef(true);

  const multitrack = useMultiTrackAudio(preview ? videoEl : null, preview ? mediaUrl(clip) : null);

  useLayoutEffect(() => {
    clearTimeout(thumbRetryTimer.current);
    thumbAttempts.current = 0;
    setDisplayDuration(clip.duration_seconds);
    setThumbBroken(false);
    regenTried.current = false;
  }, [clipKey(clip), clip.thumbnail_path, clip.thumbnail_file, clip.duration_seconds]);

  // Prefer the executable icon, then cached artwork; avoid network lookup per card.
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

  useEffect(() => () => {
    mounted.current = false;
    clearTimeout(hoverTimer.current);
    clearTimeout(thumbRetryTimer.current);
    thumbRepair.current?.();
  }, []);

  useEffect(() => {
    const observer = new IntersectionObserver(([entry]) => setVisible(entry.isIntersecting));
    if (cardRef.current) observer.observe(cardRef.current);
    return () => observer.disconnect();
  }, []);

  async function refreshMedia(): Promise<void> {
    const updated = await clipApi.snapshot(clip);
    if (!mounted.current) return;
    const current = latest.current.clip;
    if (clipKey(updated) !== clipKey(current) || (updated.media_revision ?? 0) < (current.media_revision ?? 0)) return;
    latest.current.onChanged({
      ...current,
      thumbnail_file: updated.thumbnail_file,
      thumbnail_path: updated.thumbnail_path,
      duration_seconds: updated.duration_seconds,
      media_revision: updated.media_revision,
    });
    setThumbBroken(false);
    setThumbBust(Date.now());
  }

  useEffect(() => {
    const needsThumb = !clip.thumbnail_path;
    const needsDuration = !(typeof clip.duration_seconds === "number" && Number.isFinite(clip.duration_seconds) && clip.duration_seconds > 0);
    if (!visible || (!needsThumb && !needsDuration)) return;
    let cancelled = false;
    let finish = () => {};
    const cancelQueued = enqueueMediaProbe(() => new Promise<void>((resolve) => {
      if (cancelled) { resolve(); return; }
      const video = document.createElement("video");
      video.preload = needsThumb ? "auto" : "metadata";
      video.muted = true;
      video.playsInline = true;
      let working = false;
      const timeout = setTimeout(() => { finish(); }, 10000);
      finish = () => {
        clearTimeout(timeout);
        video.onloadedmetadata = null;
        video.onloadeddata = null;
        video.onerror = null;
        video.removeAttribute("src");
        video.load();
        resolve();
      };
      const repairDuration = async () => {
        const duration = video.duration;
        if (needsDuration && Number.isFinite(duration) && duration > 0 && !sameDuration(clip.duration_seconds, duration)) {
          await clipApi.setDuration(clip, duration);
        }
      };
      const fallback = async () => {
        if (cancelled || regenTried.current) return;
        regenTried.current = true;
        const result = await clipApi.regenThumb(clip);
        if (!cancelled && result.ok) await refreshMedia();
      };
      video.onloadedmetadata = () => {
        if (cancelled || needsThumb || working) return;
        working = true;
        void repairDuration().then(() => { if (!cancelled) return refreshMedia(); }).catch((error) => console.error("Metadata refresh failed", error)).finally(finish);
      };
      video.onloadeddata = () => {
        if (cancelled || !needsThumb || working) return;
        working = true;
        void (async () => {
          await repairDuration();
          if (cancelled) return;
          const dataUrl = drawVideoThumb(video);
          if (!dataUrl) await fallback();
          else {
            const result = await clipApi.saveCapturedThumb(clip, dataUrl);
            if (!cancelled && result.ok) await refreshMedia();
          }
        })().catch((error) => console.error("Thumbnail repair failed", error)).finally(finish);
      };
      video.onerror = () => {
        if (working) return;
        working = true;
        void (needsThumb ? fallback() : Promise.resolve()).catch((error) => console.error("Thumbnail refresh failed", error)).finally(finish);
      };
      video.src = mediaUrl(clip);
      video.load();
    }));
    return () => {
      cancelled = true;
      cancelQueued();
      finish();
    };
  }, [visible, clipKey(clip), clip.video_file, clip.thumbnail_path, clip.duration_seconds, clip.media_revision]);

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

  function onThumbError() {
    if (thumbAttempts.current < 3) {
      const attempt = ++thumbAttempts.current;
      clearTimeout(thumbRetryTimer.current);
      thumbRetryTimer.current = setTimeout(() => setThumbBust(Date.now()), attempt * 1000);
      return;
    }
    if (regenTried.current) {
      setThumbBroken(true);
      return;
    }
    regenTried.current = true;
    thumbRepair.current = enqueueMediaProbe(async () => {
      if (!mounted.current || !visible) return;
      const result = await clipApi.regenThumb(clip);
      if (!mounted.current) return;
      if (result.ok) await refreshMedia();
      else setThumbBroken(true);
    });
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
      ref={cardRef}
      onContextMenu={(e) => {
        e.preventDefault();
        onContextMenu(e as unknown as MouseEvent, clip);
      }}
      onMouseEnter={enter}
      onMouseLeave={leave}
    >
      <div class="card-media">
        {showPlaceholder
          ? (
            <div class="thumb-placeholder" title="Thumbnail unavailable">
              <Icon name="film" size={34} />
            </div>
          )
          : <img class="card-thumb" src={thumbSrc!} alt="" loading="lazy" onError={onThumbError} />}

        <button class="card-open" type="button" aria-label={`Open ${clip.title || "Untitled"}`} onClick={() => onOpenDetail(clip)} />

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
      <div class="card-footer">
        <Button variant="ghost" size="sm" onClick={() => onOpenDetail(clip)}><Icon name="play" size={15} />Open clip</Button>
        <Button variant="ghost" size="icon" aria-label={`Actions for ${clip.title || "Untitled"}`}
          onClick={(e) => onContextMenu(e as unknown as MouseEvent, clip)}><Icon name="more" size={18} /></Button>
      </div>
    </div>
  );
});
