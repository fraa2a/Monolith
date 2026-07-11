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
  // Fixed port + strictPort so it matches src-tauri/tauri.conf.json's devUrl.
  server: {
    port: 1420,
    strictPort: true,
    watch: { ignored: ["**/src-tauri/**"] },
  },
  clearScreen: false,
});
