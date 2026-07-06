import { useEffect } from "preact/hooks";

interface Props {
  title: string;
  message: string;
  confirmLabel?: string;
  danger?: boolean;
  onConfirm: () => void;
  onCancel: () => void;
}

// Custom secondary confirmation popup (deno.md extra #1: delete asks again with
// a custom popup, not a native dialog).
export function ConfirmDialog(
  { title, message, confirmLabel = "Confirm", danger, onConfirm, onCancel }: Props,
) {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") onCancel();
      if (e.key === "Enter") onConfirm();
    };
    document.addEventListener("keydown", onKey, true);
    return () => document.removeEventListener("keydown", onKey, true);
  }, [onConfirm, onCancel]);

  return (
    <div class="modal-backdrop" onMouseDown={onCancel}>
      <div class="modal" onMouseDown={(e) => e.stopPropagation()}>
        <h3 class="modal-title">{title}</h3>
        <p class="modal-msg">{message}</p>
        <div class="modal-actions">
          <button class="btn" onClick={onCancel}>Cancel</button>
          <button class={danger ? "btn btn-danger" : "btn btn-primary"} onClick={onConfirm}>
            {confirmLabel}
          </button>
        </div>
      </div>
    </div>
  );
}
