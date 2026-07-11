// Simultaneous multi-track audio playback (deno.md §2.1 successor).
//
// Monolith records separate audio streams (game, mic, apps) into one file
// (see libs/encoding/mux_common.cpp). Chromium/WebView2 exposes every track
// via `video.audioTracks`, but a single <video>/<audio> element only ever
// *renders* one enabled track at a time — flipping `audioTracks[i].enabled`
// for more than one track has no effect on what you hear. The only way to
// hear every track at once is to decode each extra track on its own hidden
// element and let the OS mixer combine them acoustically, so this hook spins
// up one shadow <video> per extra track and keeps it in lockstep with the
// visible primary element (play/pause, seek, mute, volume, rate).
//
// Sync is event-driven (play/pause/seeking mirrored immediately, currentTime
// drift corrected on timeupdate), not sample-accurate — under heavy CPU
// contention drift beyond ~150ms is possible before the next correction
// tick. That's an accepted tradeoff: still strictly better than silently
// dropping every track but the first.

import { useEffect, useRef } from "preact/hooks";

const DRIFT_CORRECT_SEC = 0.15;

interface ShadowTrack {
  el: HTMLVideoElement;
}

export interface MultiTrackHandle {
  setMuted: (muted: boolean) => void;
  setVolume: (volume: number) => void;
}

// Attaches shadow tracks for `video`'s extra audio tracks once metadata is
// available. `src` must match the primary element's current src. Returns a
// handle whose setMuted/setVolume must be used instead of touching the
// primary element's `.muted`/`.volume` directly, so shadows stay in lockstep.
export function useMultiTrackAudio(
  video: HTMLVideoElement | null,
  src: string | null,
): MultiTrackHandle {
  const shadowsRef = useRef<ShadowTrack[]>([]);
  const mutedRef = useRef(true);
  const volumeRef = useRef(1);

  const applyMuted = (muted: boolean) => {
    mutedRef.current = muted;
    for (const { el } of shadowsRef.current) el.muted = muted;
  };
  const applyVolume = (volume: number) => {
    volumeRef.current = volume;
    for (const { el } of shadowsRef.current) el.volume = volume;
  };

  const teardown = () => {
    for (const { el } of shadowsRef.current) {
      el.pause();
      el.removeAttribute("src");
      try {
        el.load();
      } catch {
        /* ignore */
      }
      el.remove();
    }
    shadowsRef.current = [];
  };

  useEffect(() => {
    if (!video || !src) {
      teardown();
      return;
    }

    let cancelled = false;

    const onLoadedMetadata = () => {
      if (cancelled) return;
      teardown();
      const tracks = (video as any).audioTracks;
      const count = tracks && typeof tracks.length === "number" ? tracks.length : 0;
      if (count <= 1) return;

      // Primary element keeps only its default (first) track enabled.
      for (let i = 1; i < count; i++) {
        try {
          tracks[i].enabled = false;
        } catch {
          /* read-only in some engines */
        }
      }
      try {
        tracks[0].enabled = true;
      } catch {
        /* ignore */
      }

      const shadows: ShadowTrack[] = [];
      for (let i = 1; i < count; i++) {
        const shadow = document.createElement("video");
        shadow.style.display = "none";
        shadow.muted = mutedRef.current;
        shadow.volume = volumeRef.current;
        shadow.preload = "auto";
        shadow.playsInline = true;
        const trackIndex = i;
        shadow.onloadedmetadata = () => {
          if (cancelled) return;
          const shadowTracks = (shadow as any).audioTracks;
          if (!shadowTracks || typeof shadowTracks.length !== "number") return;
          for (let j = 0; j < shadowTracks.length; j++) {
            try {
              shadowTracks[j].enabled = j === trackIndex;
            } catch {
              /* ignore */
            }
          }
          shadow.currentTime = video.currentTime;
          if (!video.paused) shadow.play().catch(() => {});
        };
        shadow.src = src;
        shadow.load();
        document.body.appendChild(shadow);
        shadows.push({ el: shadow });
      }
      shadowsRef.current = shadows;
    };

    const syncPlay = () => {
      for (const { el } of shadowsRef.current) {
        if (Math.abs(el.currentTime - video.currentTime) > DRIFT_CORRECT_SEC) {
          el.currentTime = video.currentTime;
        }
        el.play().catch(() => {});
      }
    };
    const syncPause = () => {
      for (const { el } of shadowsRef.current) el.pause();
    };
    const syncSeek = () => {
      for (const { el } of shadowsRef.current) el.currentTime = video.currentTime;
    };
    const syncDrift = () => {
      for (const { el } of shadowsRef.current) {
        if (Math.abs(el.currentTime - video.currentTime) > DRIFT_CORRECT_SEC) {
          el.currentTime = video.currentTime;
        }
      }
    };

    video.addEventListener("loadedmetadata", onLoadedMetadata);
    video.addEventListener("play", syncPlay);
    video.addEventListener("pause", syncPause);
    video.addEventListener("seeked", syncSeek);
    video.addEventListener("timeupdate", syncDrift);

    return () => {
      cancelled = true;
      video.removeEventListener("loadedmetadata", onLoadedMetadata);
      video.removeEventListener("play", syncPlay);
      video.removeEventListener("pause", syncPause);
      video.removeEventListener("seeked", syncSeek);
      video.removeEventListener("timeupdate", syncDrift);
      teardown();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [video, src]);

  return {
    setMuted: applyMuted,
    setVolume: applyVolume,
  };
}
