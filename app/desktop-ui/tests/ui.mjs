import assert from "node:assert/strict";
import { readFile, mkdir } from "node:fs/promises";
import { spawn } from "node:child_process";
import { setTimeout as delay } from "node:timers/promises";
import { chromium } from "playwright-core";

const base = "http://127.0.0.1:4173";
const seed = JSON.parse(await readFile(new URL("../../../config/default-config.json", import.meta.url), "utf8"));
const server = spawn(process.execPath, ["node_modules/vite/bin/vite.js", "preview", "--host", "127.0.0.1", "--port", "4173", "--strictPort"], { stdio: "pipe" });
let serverError = "";
server.stderr.on("data", (chunk) => { serverError += chunk; });
let browser;
try {
  let ready = false;
  for (let attempt = 0; attempt < 100; attempt++) {
    if (server.exitCode !== null) throw new Error(`Preview exited: ${serverError}`);
    try { ready = (await fetch(base)).ok; } catch {}
    if (ready) break;
    await delay(100);
  }
  assert(ready, "Preview did not start");
  browser = await chromium.launch({ executablePath: process.env.UI_BROWSER_PATH || undefined, headless: true, args: ["--no-sandbox", "--disable-gpu"] });
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  if (process.env.UI_SCREENSHOT_DIR) await mkdir(process.env.UI_SCREENSHOT_DIR, { recursive: true });
  const errors = [];
  page.on("pageerror", (error) => errors.push(error.message));
  await page.route("**/__fixtures__/**", async (route) => {
    if (route.request().url().includes("video")) {
      return route.fulfill({ status: 204 });
    }
    const id = Number(route.request().url().match(/(\d+)/g)?.at(-1) ?? 0);
    const hue = (id * 37) % 360;
    return route.fulfill({ contentType: "image/svg+xml", body: `<svg xmlns="http://www.w3.org/2000/svg" width="640" height="360"><defs><linearGradient id="bg" x2="1" y2="1"><stop stop-color="hsl(${hue},40%,24%)"/><stop offset="1" stop-color="#101820"/></linearGradient></defs><rect width="640" height="360" fill="url(#bg)"/><path d="M0 300 160 130 270 240 410 80 640 280V360H0Z" fill="#141c23"/><text x="24" y="42" fill="#eee" font-family="sans-serif" font-size="20">UI test fixture ${id}</text></svg>` });
  });
  await page.addInitScript((config) => {
    const clips = Array.from({ length: 15 }, (_, i) => ({ id: i + 1, source: "replay", video_file: `video${i + 1}.mkv`, title: `Match ${String(i + 1).padStart(2, "0")}`, thumbnail_file: `thumb${i + 1}.svg`, video_path: `video${i + 1}.mkv`, thumbnail_path: `thumb${i + 1}.svg`, created_at_utc: "2026-10-08T10:00:00Z", duration_seconds: 30, game_process_name: "arena.exe", game_display_name: "Arena", favorite: false, hashtags: ["gameplay"], size_bytes: 12000000 }));
    const collections = [{ id: 1, name: "Best moments", color: "#c4ff42", clip_count: 3, created_at_utc: "2026-10-08T10:00:00Z" }];
    let engine = { connected: true, recording: false, replay_enabled: true, version: "1.6.2" };
    const calls = [];
    window.__uiTest = { calls, config, engine, clips, collections, failCommand: false, unknown: [] };
    let callback = 0;
    window.__TAURI_EVENT_PLUGIN_INTERNALS__ = { unregisterListener() {} };
    window.__TAURI_INTERNALS__ = {
      metadata: { currentWindow: { label: "main" }, currentWebview: { label: "main" } },
      transformCallback: () => ++callback,
      unregisterCallback() {},
      convertFileSrc: (path) => `/__fixtures__/${path}`,
      invoke: async (command, args = {}) => {
        calls.push({ command, args });
        switch (command) {
          case "list_clips": return structuredClone(clips.filter((clip) => (!args.filter.favorite || clip.favorite) && (!args.filter.search || clip.title.toLowerCase().includes(args.filter.search.toLowerCase())) && (!args.filter.game || args.filter.game === clip.game_display_name) && (!args.filter.hashtag || clip.hashtags.includes(args.filter.hashtag))));
          case "distinct_games": return ["Arena"];
          case "distinct_hashtags": return ["gameplay"];
          case "engine_status": return structuredClone(engine);
          case "get_settings": return structuredClone(config);
          case "save_settings": Object.assign(config, structuredClone(args.config)); return;
          case "runtime_status": return { monitors: [{ device: "DISPLAY1", width: 1920, height: 1080, primary: true }], available_encoders: ["h264_nvenc"], input_devices: [], audio_sessions: [] };
          case "plugin:app|version": return "1.5.0";
          case "plugin:event|listen": return callback;
          case "game_catalog_map": return {};
          case "game_icon":
          case "exe_icon": return null;
          case "game_artwork": return { icon: null, cover: null };
          case "clip_list_bookmarks": return [];
          case "clip_set_favorite": clips.find((clip) => clip.id === args.id).favorite = args.favorite; return;
          case "list_collections": return structuredClone(collections);
          case "collection_clips": return structuredClone(clips.slice(0, 3));
          case "create_collection": { const id = Math.max(...collections.map((c) => c.id), 0) + 1; collections.push({ id, name: args.name, color: args.color, clip_count: 0 }); return id; }
          case "rename_collection": collections.find((c) => c.id === args.id).name = args.name; return;
          case "delete_collection": collections.splice(collections.findIndex((c) => c.id === args.id), 1); return;
          case "recorder_command":
            if (window.__uiTest.failCommand) throw new Error("Recorder unavailable");
            if (args.method === "recording_start") engine.recording = true;
            if (args.method === "recording_stop") engine.recording = false;
            return;
          default:
            if (command.startsWith("plugin:window|") || command === "plugin:event|unlisten") return;
            window.__uiTest.unknown.push(command);
            throw new Error(`Unexpected mock command: ${command}`);
        }
      },
    };
  }, seed);
  await page.goto(base);
  async function until(fn, label) {
    for (let attempt = 0; attempt < 100; attempt++) {
      if (await fn()) return;
      await delay(50);
    }
    throw new Error(`Timed out: ${label}`);
  }
  const count = () => page.locator(".card").count();
  await until(async () => await count() === 15, "initial library");
  if (process.env.UI_SCREENSHOT_DIR) await page.screenshot({ path: `${process.env.UI_SCREENSHOT_DIR}/library-initial.png` });
  assert.equal(await page.locator(".grid").evaluate((el) => getComputedStyle(el).gridTemplateColumns.split(" ").length), 5);
  await page.getByRole("searchbox", { name: "Search clips" }).fill("Match 03");
  await until(async () => await count() === 1, "clip search");
  await page.getByRole("button", { name: "Clear filters" }).click();
  await until(async () => await count() === 15, "clear search");
  await page.getByRole("button", { name: "Filter by game" }).click();
  await page.locator(".filter-list").getByRole("button", { name: "Arena", exact: true }).click();
  await until(async () => await page.getByRole("button", { name: "Clear filters" }).count() === 1, "game filter");
  await page.getByRole("button", { name: "Clear filters" }).click();
  await page.locator(".card").first().hover();
  await page.locator(".card").first().getByTitle("Add to favorites").click();
  await page.getByRole("button", { name: "Favorites", exact: true }).click();
  await until(async () => await count() === 1, "favorite navigation");
  await page.getByRole("button", { name: "Collections", exact: true }).click();
  await until(async () => await page.getByRole("button", { name: "Best moments 3 clips" }).count() === 1, "collections");
  await page.getByRole("button", { name: "Library", exact: true }).click();
  await until(async () => await count() === 15, "return from collections");
  await page.getByRole("button", { name: "Open Match 01", exact: true }).focus();
  await page.keyboard.press("Enter");
  await until(async () => await page.locator(".detail").count() === 1, "keyboard clip opening");
  await page.keyboard.press("Escape");
  await until(async () => await count() === 15, "detail close");
  await page.getByRole("button", { name: "Collections", exact: true }).click();
  await page.getByRole("button", { name: "New collection", exact: true }).first().click();
  await page.getByPlaceholder("Collection name").fill("Highlights");
  await page.getByRole("button", { name: "Create", exact: true }).click();
  await until(async () => await page.getByRole("button", { name: "Rename Highlights" }).count() === 1, "create collection");
  await page.getByRole("button", { name: "Rename Highlights" }).click();
  await page.getByPlaceholder("Collection name").fill("Highlights 2026");
  await page.getByRole("button", { name: "Save", exact: true }).click();
  await until(async () => await page.getByRole("button", { name: "Delete Highlights 2026" }).count() === 1, "rename collection");
  await page.getByRole("button", { name: "Delete Highlights 2026" }).click();
  await until(async () => await page.getByRole("alertdialog").count() === 1, "delete dialog");
  await until(async () => await page.locator(":focus").textContent() === "Cancel", "initial cancel focus");
  await page.keyboard.press("Enter");
  assert.equal(await page.evaluate(() => window.__uiTest.calls.filter((c) => c.command === "delete_collection").length), 0, "Enter on Cancel must not delete");
  await page.getByRole("button", { name: "Delete Highlights 2026" }).click();
  await until(async () => await page.locator(":focus").textContent() === "Cancel", "cancel focus");
  await page.keyboard.press("Shift+Tab");
  assert.equal(await page.locator(":focus").textContent(), "Delete");
  await page.keyboard.press("Tab");
  assert.equal(await page.locator(":focus").textContent(), "Cancel");
  await page.keyboard.press("Tab");
  await page.keyboard.press("Enter");
  await until(async () => await page.getByRole("button", { name: "Delete Highlights 2026" }).count() === 0, "delete collection");
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  const nav = page.getByRole("navigation", { name: "Settings pages" });
  for (const name of ["General", "Capture", "Audio", "Game Detection", "Recording", "Clip", "Hotkeys", "Advanced"]) {
    await nav.getByRole("button", { name, exact: true }).click();
    assert.equal(await page.getByRole("heading", { level: 1, name, exact: true }).count(), 1);
  }
  await page.getByRole("searchbox", { name: "Search settings pages" }).fill("Hotkeys");
  assert.equal(await nav.locator(".settings-tab").count(), 1);
  await page.getByRole("searchbox", { name: "Search settings pages" }).fill("");
  await nav.getByRole("button", { name: "Capture", exact: true }).click();
  await page.locator(".set-field").filter({ hasText: "Resolution" }).locator("select").selectOption("720p");
  await until(async () => await page.evaluate(() => window.__uiTest.config.capture.resolution_preset) === "720p", "settings save");
  await nav.getByRole("button", { name: "Hotkeys", exact: true }).click();
  await page.locator(".set-field").filter({ hasText: "Save replay" }).locator("button").click();
  await page.keyboard.press("Control+Shift+F7");
  await until(async () => await page.evaluate(() => window.__uiTest.config.hotkeys.save_replay) === "Ctrl+Shift+F7", "shortcut save");
  await page.getByRole("button", { name: "Close settings" }).click();
  await until(async () => (await page.getByTitle("Save an instant replay").textContent()).includes("Ctrl+Shift+F7"), "titlebar shortcut refresh");
  await page.getByTitle("Save an instant replay").click();
  await page.getByRole("button", { name: /Long recording/ }).click();
  await until(async () => await page.getByRole("button", { name: /Stop recording/ }).count() === 1, "start recording");
  assert((await page.getByRole("button", { name: /Stop recording/ }).textContent()).includes("Ctrl+Shift+F10"));
  await page.getByRole("button", { name: /Stop recording/ }).click();
  await until(async () => await page.getByRole("button", { name: /Long recording/ }).count() === 1, "stop recording");
  await page.evaluate(() => { window.__uiTest.failCommand = true; });
  await page.getByTitle("Save an instant replay").click();
  await until(async () => await page.getByText("Error: Recorder unavailable", { exact: true }).count() === 1, "recorder error");
  await page.locator(".connect-toast").getByTitle("Dismiss").click();
  await page.evaluate(() => { window.__uiTest.failCommand = false; window.__uiTest.engine.connected = false; });
  await until(async () => await page.getByTitle("Save an instant replay").isDisabled(), "disconnected controls");
  await page.evaluate(() => { window.__uiTest.engine.connected = true; });
  await until(async () => !(await page.getByTitle("Save an instant replay").isDisabled()), "reconnected controls");
  await page.getByRole("button", { name: "Library", exact: true }).click();
  await until(async () => await count() === 15, "library after settings");
  if (process.env.UI_SCREENSHOT_DIR) await mkdir(process.env.UI_SCREENSHOT_DIR, { recursive: true });
  for (const width of [1920, 1280, 960, 800]) {
    await page.setViewportSize({ width, height: 1080 });
    for (const view of ["Library", "Collections", "Settings"]) {
      await page.getByRole("button", { name: view, exact: true }).click();
      await until(async () => view === "Library" ? await count() === 15 : view === "Collections" ? await page.locator(".collections-grid").count() === 1 : await page.locator(".settings-body").count() === 1, `${view} layout`);
      if (view === "Settings") await nav.getByRole("button", { name: "Capture", exact: true }).click();
      assert.equal(await page.locator(".sidebar [aria-current=page]").count(), 1, "One active main page");
      assert.equal(await page.getByRole("button", { name: view, exact: true }).getAttribute("aria-current"), "page");
      if (view === "Settings") assert((await page.locator(".settings-search").boundingBox()).height < 50, "Settings search must keep its control height");
      if (view === "Collections") {
        const covers = await page.locator(".collection-cover").all();
        assert(Math.abs((await covers[0].boundingBox()).y - (await covers[1].boundingBox()).y) < 1, "Collection covers align");
      }
      const overflow = await page.evaluate(() => [...document.querySelectorAll(".titlebar, .content, .settings-nav, .settings-body")].filter((el) => el.scrollWidth > el.clientWidth + 1).map((el) => el.className));
      assert.deepEqual(overflow, [], `${view} overflow at ${width}`);
      if (process.env.UI_SCREENSHOT_DIR) {
        await page.mouse.move(width - 20, 1060);
        await page.evaluate(() => new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve))));
        await page.screenshot({ path: `${process.env.UI_SCREENSHOT_DIR}/${view.toLowerCase()}-${width}.png` });
      }
    }
  }
  assert.deepEqual(errors, [], "No browser exceptions");
  assert.deepEqual(await page.evaluate(() => window.__uiTest.unknown), [], "All fixture calls are handled");
  const methods = await page.evaluate(() => window.__uiTest.calls.filter((c) => c.command === "recorder_command").map((c) => c.args.method));
  assert(methods.includes("save_replay") && methods.includes("recording_start") && methods.includes("recording_stop"));
  console.log("PASS: library filters, favorites, clip keyboard access, collection CRUD and cancellation, settings pages/search/save, shortcuts, capture commands/errors/connection, and 12 responsive layouts.");
} finally {
  await browser?.close();
  server.kill();
}
