// Bundles the Preact frontend (ui/src/main.tsx) into dist/ with esbuild, using
// the Deno loader so npm:/jsr: specifiers from deno.json resolve. Run via
// `deno task ui:build`; `deno task compile` runs this then embeds dist/ into the
// shipped Monolith.UI.exe.

import * as esbuild from "esbuild";
import { denoPlugins } from "esbuild-deno-loader";
import { join, toFileUrl } from "@std/path";

const root = import.meta.dirname!;
const dist = join(root, "dist");

await Deno.mkdir(join(dist, "assets"), { recursive: true });

await esbuild.build({
  plugins: [...denoPlugins({ configPath: join(root, "deno.json") })],
  // esbuild-deno-loader resolves entry points as URLs; an absolute Windows path
  // must be a file:// URL or the deno resolver rejects it.
  entryPoints: [toFileUrl(join(root, "ui", "src", "main.tsx")).href],
  bundle: true,
  format: "esm",
  outfile: join(dist, "assets", "app.js"),
  jsx: "automatic",
  jsxImportSource: "preact",
  minify: true,
  target: ["chrome110"], // WebView2 is evergreen Edge/Chromium
});

await Deno.copyFile(join(root, "ui", "index.html"), join(dist, "index.html"));
await Deno.copyFile(join(root, "ui", "styles.css"), join(dist, "assets", "styles.css"));

esbuild.stop();
console.log("UI bundled -> dist/");
