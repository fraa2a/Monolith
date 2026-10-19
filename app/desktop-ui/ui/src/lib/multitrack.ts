// WebView2 plays the first audio track. Hidden elements select additional tracks
// by fragment and follow the visible video clock; unavailable tracks are ignored.

import { useEffect, useRef, useState } from "preact/hooks";

const DRIFT_CORRECT_SEC = 0.15;

interface ShadowTrack {
  el: HTMLVideoElement;
}

export interface MultiTrackHandle {
  unsupported: boolean;
  setMuted: (muted: boolean) => void;
  setVolume: (volume: number) => void;
}

// Attach extra audio elements after metadata is available; cleanup releases them.
export function useMultiTrackAudio(
  video: HTMLVideoElement | null,
  src: string | null,
): MultiTrackHandle {
  const [unsupported, setUnsupported] = useState(false);
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
    setUnsupported(!(video as any).audioTracks);
    mutedRef.current = video.muted;
    volumeRef.current = video.volume;

    const onLoadedMetadata = () => {
      if (cancelled) return;
      teardown();
      const tracks = (video as any).audioTracks;
      setUnsupported(!tracks);
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
        shadow.playbackRate = video.playbackRate;
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
          shadow.playbackRate = video.playbackRate;
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

    const syncVolume = () => {
      applyMuted(video.muted);
      applyVolume(video.volume);
    };
    const syncRate = () => {
      for (const { el } of shadowsRef.current) el.playbackRate = video.playbackRate;
    };
    video.addEventListener("volumechange", syncVolume);
    video.addEventListener("ratechange", syncRate);
    video.addEventListener("loadedmetadata", onLoadedMetadata);
    if (video.readyState >= HTMLMediaElement.HAVE_METADATA) onLoadedMetadata();
    video.addEventListener("play", syncPlay);
    video.addEventListener("pause", syncPause);
    video.addEventListener("seeked", syncSeek);
    video.addEventListener("timeupdate", syncDrift);

    return () => {
      cancelled = true;
      video.removeEventListener("volumechange", syncVolume);
      video.removeEventListener("ratechange", syncRate);
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
    unsupported,
    setMuted: applyMuted,
    setVolume: applyVolume,
  };
}
