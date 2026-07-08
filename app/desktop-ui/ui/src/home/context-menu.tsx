import { useEffect, useRef } from "preact/hooks";
import type { Clip } from "../lib/api.ts";
import { Icon } from "../shell/icons.tsx";

export type MenuAction = "favorite" | "hashtag" | "fullscreen" | "delete";

interface Props {
  x: number;
  y: number;
  clip: Clip;
  onAction: (action: MenuAction, clip: Clip) => void;
  onClose: () => void;
}

export function ContextMenu({ x, y, clip, onAction, onClose }: Props) {
  const ref = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    const onDown = (e: MouseEvent) => {
      if (ref.current && !ref.current.contains(e.target as Node)) onClose();
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") onClose();
    };
    document.addEventListener("mousedown", onDown, true);
    document.addEventListener("keydown", onKey, true);
    return () => {
      document.removeEventListener("mousedown", onDown, true);
      document.removeEventListener("keydown", onKey, true);
    };
  }, [onClose]);

  const style = {
    left: `${Math.min(x, globalThis.innerWidth - 210)}px`,
    top: `${Math.min(y, globalThis.innerHeight - 200)}px`,
  };

  const act = (a: MenuAction) => () => {
    onAction(a, clip);
    if (a !== "delete" && a !== "hashtag") onClose();
  };

  return (
    <div class="ctx-menu" ref={ref} style={style} role="menu">
      <button class={`ctx-item ctx-favorite ${clip.favorite ? "active" : ""}`} onClick={act("favorite")}>
        <span class="ctx-ico"><Icon name="star" size={15} filled={clip.favorite} /></span>
        {clip.favorite ? "Remove favorite" : "Add to favorites"}
      </button>
      <button class="ctx-item" onClick={act("hashtag")}>
        <span class="ctx-ico"><Icon name="hash" size={15} /></span>Hashtags...
      </button>
      <button class="ctx-item" onClick={act("fullscreen")}>
        <span class="ctx-ico"><Icon name="maximize" size={15} /></span>Fullscreen
      </button>
      <div class="ctx-sep" />
      <button class="ctx-item ctx-danger" onClick={act("delete")}>
        <span class="ctx-ico"><Icon name="trash-2" size={15} /></span>Delete
      </button>
    </div>
  );
}
