import { type Clip, clipApi } from "../lib/api.ts";

export function AudioTrackNotice({ clip }: { clip: Clip }) {
  return (
    <div class="audio-track-notice" role="status">
      <span>This player cannot select additional audio tracks. Open the video in an external player to hear all tracks.</span>
      <button class="btn btn-ghost" onClick={() => { void clipApi.revealInExplorer(clip); }}>Show video file</button>
    </div>
  );
}
