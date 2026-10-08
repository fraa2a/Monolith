// Video player helpers shared by the card preview and the detail player.
// See multitrack.ts for simultaneous multi-track audio playback.

// Clear the old source before reusing an element for another clip.
export function resetPlayer(video: HTMLVideoElement | null): void {
  if (!video) return;
  video.pause();
  video.muted = true;
  try {
    video.currentTime = 0;
  } catch {
    /* not seekable yet */
  }
  video.removeAttribute("src");
  try {
    video.load();
  } catch {
    /* ignore */
  }
}
