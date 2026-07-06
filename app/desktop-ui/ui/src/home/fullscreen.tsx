import { useEffect } from "preact/hooks";
import { type Clip, mediaUrl } from "../lib/api.ts";

interface Props {
  clip: Clip;
  onClose: () => void;
}

// Fullscreen playback at original framerate (deno.md §2.1). Full-window overlay
// with native <video> controls; Esc closes.
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
      <button class="fs-close" onClick={onClose} title="Close (Esc)">×</button>
      <video class="fs-video" src={mediaUrl(clip)} controls autoPlay />
    </div>
  );
}
