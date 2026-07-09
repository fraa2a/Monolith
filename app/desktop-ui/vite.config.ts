import { defineConfig } from "vite";
import preact from "@preact/preset-vite";

export default defineConfig({
  root: "ui",
  plugins: [preact()],
  build: {
    outDir: "../dist",
    emptyOutDir: true,
    target: "chrome110", // WebView2 is evergreen Edge/Chromium
  },
});
