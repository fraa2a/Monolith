import { useEffect } from "preact/hooks";
import { type Clip, mediaUrl } from "../lib/api.ts";
import { Icon } from "../shell/icons.tsx";
import { enableAllAudioTracks } from "../lib/player.ts";

interface Props {
  clip: Clip;
  onClose: () => void;
}

// Fullscreen playback at original framerate. Full-window overlay with native
// <video> controls; Esc closes. All audio tracks are enabled so playback matches
// the recorded file.
export function Fullscreen({ clip, onClose }: Props) {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") onClose();
    };
    document.addEventListener("keydown", onKey, true);
    return () => document.removeEventListener("keydown", onKey, true);
  }, [onClose]);

  return (
    <div class="fs-backdrop">
      <button class="fs-close" onClick={onClose} title="Close (Esc)">
        <Icon name="x" size={20} />
      </button>
      <video
        class="fs-video"
        src={mediaUrl(clip)}
        controls
        autoPlay
        onLoadedMetadata={(e) => enableAllAudioTracks(e.currentTarget)}
      />
    </div>
  );
}
