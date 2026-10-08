import type { JSX } from "preact";

type Variant = "default" | "secondary" | "outline" | "ghost" | "destructive";
type Props = JSX.ButtonHTMLAttributes<HTMLButtonElement> & {
  variant?: Variant;
  size?: "default" | "sm" | "icon";
};

const variants: Record<Variant, string> = {
  default: "btn-primary",
  secondary: "btn-secondary",
  outline: "btn-outline",
  ghost: "btn-ghost",
  destructive: "btn-danger",
};

export function Button({ variant = "default", size = "default", class: className = "", type = "button", ...props }: Props) {
  return <button {...props} type={type} data-slot="button" class={`btn ${variants[variant]} btn-${size} ${className}`} />;
}
