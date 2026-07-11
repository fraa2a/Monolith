import { useEffect, useRef, useState } from "preact/hooks";
import { type Clip, mediaUrl } from "../lib/api.ts";
import { Icon } from "../shell/icons.tsx";
import { useMultiTrackAudio, type MultiTrackHandle } from "../lib/multitrack.ts";
import { appWindow } from "../lib/window.ts";

interface Props {
  clip: Clip;
  initialTime?: number;
  onClose: () => void;
}

const SKIP_SECONDS = 10;
const HIDE_DELAY_MS = 2200;

function fmtTime(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) seconds = 0;
  const m = Math.floor(seconds / 60);
  const s = Math.floor(seconds % 60);
  return `${m}:${s < 10 ? "0" : ""}${s}`;
}

// Custom YouTube-style fullscreen chrome (not the native <video controls>),
// driven by an externally-owned <video> element rather than one it renders
// itself — this lets a caller that already has a playing element (e.g. the
// detail view) reuse it in place instead of restarting playback on a new one.
export function FullscreenChrome(
  { videoEl, multitrack, onClose }: {
    videoEl: HTMLVideoElement | null;
    multitrack: MultiTrackHandle;
    onClose: () => void;
  },
) {
  const [playing, setPlaying] = useState(true);
  const [current, setCurrent] = useState(0);
  const [duration, setDuration] = useState(0);
  const [volume, setVolume] = useState(1);
  const [muted, setMuted] = useState(false);
  const [controlsVisible, setControlsVisible] = useState(true);
  const hideTimer = useRef<number | undefined>(undefined);

  useEffect(() => {
    if (!videoEl) return;
    setDuration(videoEl.duration || 0);
    setCurrent(videoEl.currentTime || 0);
    setPlaying(!videoEl.paused);
    setMuted(videoEl.muted);
    setVolume(videoEl.volume);

    const onLoadedMetadata = () => setDuration(videoEl.duration || 0);
    const onTimeUpdate = () => setCurrent(videoEl.currentTime);
    const onPlay = () => setPlaying(true);
    const onPause = () => setPlaying(false);
    videoEl.addEventListener("loadedmetadata", onLoadedMetadata);
    videoEl.addEventListener("timeupdate", onTimeUpdate);
    videoEl.addEventListener("play", onPlay);
    videoEl.addEventListener("pause", onPause);
    return () => {
      videoEl.removeEventListener("loadedmetadata", onLoadedMetadata);
      videoEl.removeEventListener("timeupdate", onTimeUpdate);
      videoEl.removeEventListener("play", onPlay);
      videoEl.removeEventListener("pause", onPause);
    };
  }, [videoEl]);

  function scheduleHide() {
    window.clearTimeout(hideTimer.current);
    hideTimer.current = window.setTimeout(() => setControlsVisible(false), HIDE_DELAY_MS);
  }
  function wake() {
    setControlsVisible(true);
    scheduleHide();
  }
  useEffect(() => {
    scheduleHide();
    return () => window.clearTimeout(hideTimer.current);
  }, []);

  function togglePlay() {
    const v = videoEl;
    if (!v) return;
    if (v.paused) v.play(); else v.pause();
  }
  function skip(delta: number) {
    const v = videoEl;
    if (!v) return;
    v.currentTime = Math.min(Math.max(v.currentTime + delta, 0), v.duration || v.currentTime + delta);
  }
  function toggleMute() {
    const v = videoEl;
    if (!v) return;
    const next = !v.muted;
    v.muted = next;
    setMuted(next);
    multitrack.setMuted(next);
  }
  function onVolume(e: Event) {
    const val = Number((e.target as HTMLInputElement).value);
    setVolume(val);
    if (videoEl) videoEl.volume = val;
    multitrack.setVolume(val);
    if (val > 0 && muted) {
      if (videoEl) videoEl.muted = false;
      setMuted(false);
      multitrack.setMuted(false);
    }
  }
  function onSeek(e: Event) {
    const t = Number((e.target as HTMLInputElement).value);
    if (videoEl) videoEl.currentTime = t;
    setCurrent(t);
  }

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") { onClose(); return; }
      if (e.key === " " || e.key === "Spacebar") { e.preventDefault(); togglePlay(); wake(); return; }
      if (e.key === "ArrowLeft") { skip(-SKIP_SECONDS); wake(); return; }
      if (e.key === "ArrowRight") { skip(SKIP_SECONDS); wake(); return; }
      if (e.key === "m" || e.key === "M") { toggleMute(); wake(); return; }
    };
    document.addEventListener("keydown", onKey, true);
    return () => document.removeEventListener("keydown", onKey, true);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [onClose, videoEl, muted]);

  // Document-level (not a wrapping div's onMouseMove) so the autohide timer
  // resets no matter which sibling element the pointer is over — the chrome
  // here only renders fixed-position overlays, not a full-viewport container.
  useEffect(() => {
    document.addEventListener("mousemove", wake);
    return () => document.removeEventListener("mousemove", wake);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return (
    <>
      <button
        class={`fs-close ${controlsVisible ? "" : "fs-controls-hidden"}`}
        onClick={(e) => { e.stopPropagation(); onClose(); }}
        title="Close (Esc)"
      >
        <Icon name="x" size={20} />
      </button>
      <div
        class={`fs-controls ${controlsVisible ? "" : "fs-controls-hidden"}`}
        onClick={(e) => e.stopPropagation()}
      >
        <input
          class="timeline fs-timeline"
          type="range"
          min={0}
          max={duration || 0}
          step={0.1}
          value={current}
          onInput={onSeek}
        />
        <div class="fs-controls-row">
          <div class="fs-controls-left">
            <button class="fs-btn" onClick={togglePlay} title={playing ? "Pause (Space)" : "Play (Space)"}>
              <Icon name={playing ? "pause" : "play"} size={18} />
            </button>
            <button class="fs-btn" onClick={() => skip(-SKIP_SECONDS)} title="Back 10s (←)">
              <Icon name="rotate-ccw" size={17} />
            </button>
            <button class="fs-btn" onClick={() => skip(SKIP_SECONDS)} title="Forward 10s (→)">
              <Icon name="rotate-cw" size={17} />
            </button>
            <span class="time fs-time">{fmtTime(current)} / {fmtTime(duration)}</span>
          </div>
          <div class="fs-controls-right vol">
            <button class="fs-btn" onClick={toggleMute} title={muted ? "Unmute (M)" : "Mute (M)"}>
              <Icon name={muted ? "volume-x" : "volume-2"} size={17} />
            </button>
            <input
              class="vol-slider"
              type="range"
              min={0}
              max={1}
              step={0.01}
              value={muted ? 0 : volume}
              onInput={onVolume}
            />
            <button class="fs-btn" onClick={(e) => { e.stopPropagation(); onClose(); }} title="Exit fullscreen (Esc)">
              <Icon name="minimize" size={17} />
            </button>
          </div>
        </div>
      </div>
    </>
  );
}

interface FullscreenProps {
  clip: Clip;
  initialTime?: number;
  onClose: () => void;
}

// Fullscreen playback at original framerate, in a custom YouTube-style frame.
// The window is maximized for the duration of playback and restored on close.
// Owns its own <video> since there is no existing playing element to reuse
// (triggered from a card hover-preview, a different element/component); seeds
// currentTime from the preview's position so it continues near where the
// preview left off instead of visibly restarting from 0.
export function Fullscreen({ clip, initialTime = 0, onClose }: FullscreenProps) {
  const [videoEl, setVideoEl] = useState<HTMLVideoElement | null>(null);

  const multitrack = useMultiTrackAudio(videoEl, mediaUrl(clip));

  useEffect(() => {
    appWindow.maximize();
    return () => { appWindow.unmaximize(); };
  }, []);

  return (
    <div class="fs-backdrop">
      <video
        class="fs-video"
        ref={setVideoEl}
        src={mediaUrl(clip)}
        autoPlay
        onLoadedMetadata={(e) => {
          const v = e.currentTarget;
          if (initialTime > 0) v.currentTime = initialTime;
        }}
        onClick={(e) => {
          e.stopPropagation();
          const v = e.currentTarget;
          v.paused ? v.play() : v.pause();
        }}
      />
      <FullscreenChrome videoEl={videoEl} multitrack={multitrack} onClose={onClose} />
    </div>
  );
}
