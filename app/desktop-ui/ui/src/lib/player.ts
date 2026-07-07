// Video player helpers shared by the card preview and the detail player.

// Enables every audio track on a <video> so playback reflects the real file.
// Monolith records multiple audio tracks (game, mic, apps), but browsers play
// only the first enabled track by default. Chromium/WebView2 exposes the tracks
// via `video.audioTracks`; when that API is unavailable we simply leave the
// default track playing and document the limitation.
export function enableAllAudioTracks(video: HTMLVideoElement): void {
  // deno-lint-ignore no-explicit-any
  const tracks = (video as any).audioTracks;
  if (!tracks || typeof tracks.length !== "number") return;
  for (let i = 0; i < tracks.length; i++) {
    try {
      tracks[i].enabled = true;
    } catch {
      /* read-only in some engines; ignore */
    }
  }
}

// Fully resets a <video> element's audio/playback state before it is reused for
// a different clip, so leftover audio from the previous clip can't bleed through
// during the first silent seconds of the next one.
export function resetPlayer(video: HTMLVideoElement | null): void {
  if (!video) return;
  video.pause();
  video.muted = true;
  try {
    video.currentTime = 0;
  } catch {
    /* not seekable yet */
  }
  // Detach the source and reload so decoder/audio state is cleared.
  video.removeAttribute("src");
  try {
    video.load();
  } catch {
    /* ignore */
  }
}
