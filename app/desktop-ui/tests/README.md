# Frontend UI regression check

From `app/desktop-ui`:

```sh
npm ci --ignore-scripts --no-audit
npx tsc --noEmit
npx vite build
npx playwright-core install --with-deps chromium
npm run test:ui
```

The check starts its own Vite production preview on port 4173 and closes it on
completion. `UI_BROWSER_PATH` can select an existing Chromium executable.
`UI_SCREENSHOT_DIR` optionally saves screenshots outside the repository.

The fixture intercepts Tauri calls before loading the app. It exercises library
search and filters, favorite navigation, keyboard clip opening, returning from
collections, collection create/rename/delete, cancellation and focus containment,
all eight settings pages, settings search, debounced writes, hotkey capture and
refresh, recorder commands, errors and disconnected controls. It checks Library,
Collections and Capture settings for horizontal overflow at 1920, 1280, 960 and
800 pixels.

Fixture thumbnails are generated test data, not shipped app content. Media
requests return an empty response. This check does not validate decoding,
recording devices, Windows dialogs, native window behavior or WebView2. Those
still need a Windows runtime smoke test.
