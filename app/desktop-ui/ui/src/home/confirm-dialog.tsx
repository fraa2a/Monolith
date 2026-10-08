import { useLayoutEffect, useRef } from "preact/hooks";
import { Button } from "../components/ui/button.tsx";

interface Props {
  title: string;
  message: string;
  confirmLabel?: string;
  danger?: boolean;
  onConfirm: () => void;
  onCancel: () => void;
}

export function ConfirmDialog(
  { title, message, confirmLabel = "Confirm", danger, onConfirm, onCancel }: Props,
) {
  const modal = useRef<HTMLDivElement>(null);
  useLayoutEffect(() => {
    const previous = document.activeElement as HTMLElement | null;
    modal.current?.querySelector<HTMLButtonElement>("button")?.focus();
    return () => previous?.focus();
  }, []);

  useLayoutEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        e.preventDefault();
        onCancel();
      }
      if (e.key === "Tab") {
        const buttons = modal.current?.querySelectorAll<HTMLButtonElement>("button:not(:disabled)");
        if (!buttons?.length) return;
        const next = e.shiftKey ? buttons[buttons.length - 1] : buttons[0];
        const edge = e.shiftKey ? buttons[0] : buttons[buttons.length - 1];
        if (document.activeElement === edge) {
          e.preventDefault();
          next.focus();
        }
      }
    };
    document.addEventListener("keydown", onKey, true);
    return () => document.removeEventListener("keydown", onKey, true);
  }, [onCancel]);

  return (
    <div class="modal-backdrop" onMouseDown={onCancel}>
      <div ref={modal} class="modal" role="alertdialog" aria-modal="true" aria-labelledby="confirm-title" aria-describedby="confirm-message" onMouseDown={(e) => e.stopPropagation()}>
        <h3 class="modal-title" id="confirm-title">{title}</h3>
        <p class="modal-msg" id="confirm-message">{message}</p>
        <div class="modal-actions">
          <Button variant="outline" onClick={onCancel}>Cancel</Button>
          <Button variant={danger ? "destructive" : "default"} onClick={onConfirm}>
            {confirmLabel}
          </Button>
        </div>
      </div>
    </div>
  );
}
