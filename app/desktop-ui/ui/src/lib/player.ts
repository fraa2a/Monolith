// Video player helpers shared by the card preview and the detail player.
// See multitrack.ts for simultaneous multi-track audio playback.

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
