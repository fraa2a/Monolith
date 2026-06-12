Status note — 2026-06-12:

This file is historical. The missing `Monolith.Settings.exe` problem has been fixed, and the Settings app has been manually tested by the user. Settings opens, saves configuration, persists across restarts, and implemented settings behave correctly. Audio V2 foundation has since been added; remaining audio work is mixer/runtime validation.

Current state:
Monolith was updated to use WinUI 3 for Settings, and the Settings work is partially implemented. The main recorder app builds and runs, but when opening Settings it fails because Monolith.Settings.exe is missing from the build/output folder.

Goal:
Fix the missing Monolith.Settings.exe build/output problem and complete the WinUI 3 Settings implementation end-to-end, without breaking replay saving, manual recording, paths, branding, or existing recorder behavior.

Start by inspecting:

* root CMakeLists.txt / solution/build files
* app/recorder build setup
* any app/settings, Monolith.Settings, WinUI, .csproj, .vcxproj, .sln, or packaging-related files
* GitHub Actions workflows
* config/default-config.json
* existing AppData\Local\Monolith config logic
* docs/handover/ACTIVE_HANDOVER.md
* docs/ROADMAP.md
* docs/ARCHITECTURE.md
* handoff.txt
* the recorder code that launches Settings from the tray/menu

Main tasks:

1. Diagnose why Monolith.Settings.exe is missing

* Find whether the Settings project exists but is not included in the build.
* Find whether it builds into a different output directory.
* Find whether the recorder is looking in the wrong path.
* Find whether GitHub Actions/build scripts only build the recorder target and skip Settings.
* Find whether WinUI 3 packaging/output settings are misconfigured.
* Report the root cause before fixing it.

2. Fix Settings build integration

* Ensure Monolith.Settings.exe is actually built.
* Ensure it is included in the normal local build output.
* Ensure it is included in CI/GitHub Actions artifacts if artifacts are produced.
* If the repo uses CMake as the top-level build system, integrate the Settings build cleanly without messy hardcoded local paths.
* If the Settings project is MSBuild/.csproj-based, add the correct build step/project reference or documented build command.
* Do not require manual copying after every build.
* Do not rely on absolute paths like D:\Scatola\Coding\Monolith.
* Keep the build reproducible on a clean machine with the documented dependencies.

3. Fix Settings launch from recorder

* The tray/menu Settings entry must launch the correct Monolith.Settings.exe.
* The recorder should resolve the Settings executable robustly:

  * first check the same directory as the recorder executable;
  * if needed, support a clear development-build fallback;
  * never hardcode a user-specific path.
* If Settings is missing, show/log a useful error instead of failing silently or crashing.
* Make sure the Settings menu item is enabled only when the app can reasonably launch Settings, or show a clear error if not found.
* Avoid spawning multiple duplicate Settings windows if one is already open, if this can be handled simply.

4. Complete persistent settings storage

* Settings must persist across restarts.
* Use the existing JSON config approach with nlohmann-json if already added.
* Store user settings under AppData\Local\Monolith, for example:
  AppData\Local\Monolith\config.json
* Load config/default-config.json as defaults.
* Merge user config over defaults.
* Handle missing keys safely.
* Handle invalid/corrupt user config safely:

  * log the problem;
  * preserve or back up the invalid file if reasonable;
  * fall back to defaults without crashing.
* Do not store user media files in AppData by default.
* Preserve current defaults:

  * clips: Videos\Monolith\Clips
  * recordings: Videos\Monolith\Recordings
  * logs/runtime/cache/configs: AppData\Local\Monolith

5. Complete the WinUI 3 Settings UI
   Implement a functional, simple Settings window. It does not need to be visually perfect, but it must be real and wired.

Settings pages/sections should include at least:

General:

* App name/branding should say Monolith.
* Show current config file path.
* Show current app/runtime paths if useful.

Output paths:

* Replay clips folder.
* Manual recordings folder.
* Buttons to browse/select folders.
* Buttons to reset paths to defaults.
* Validate selected folders.
* Create missing folders when saving/applying, if safe.

Clip:

* Replay buffer duration/length, only if currently supported by runtime/config.
* Replay clip output folder.
* Replay hotkey display.
* Hotkey editing only if it can be implemented safely end-to-end; otherwise keep hotkeys read-only and clearly mark rebinding as future work.

Manual recording:

* Recording output folder.
* Recording format display: MKV/MP4.
* Pause behavior display: continuous output with paused time omitted.
* Manual recording hotkey display.
* Do not expose fake controls that are not wired.

Capture / Video / Encoder:

* Add real capture/video settings similar to OBS-style basic output controls, but only implement what can be safely wired.
* Allow selecting which monitor/display to record.

  * Detect available monitors/displays.
  * Show a readable monitor name/index/resolution.
  * Save the selected monitor in config.
  * The recorder must use the selected monitor on next capture start/restart.
  * If live monitor switching is unsafe, mark it as restart-required.
* Add output resolution settings.

  * Allow recording at native resolution or a custom scaled resolution.
  * Include common presets such as 1280x720, 1920x1080, 2560x1440, and native/source resolution.
  * Do not assume every user wants 1080p.
  * Validate resolution values and prevent invalid sizes.
  * Make sure scaling is handled in the encoding path if capture resolution differs from output resolution.
* Add bitrate settings.

  * Expose video bitrate in kbps or Mbps.
  * Use safe defaults.
  * Validate min/max values.
  * Make the selected bitrate actually affect the encoder.
* Add encoder/backend settings.

  * Allow selecting the available encoder backend when supported, for example AMD AMF, NVENC, QSV, or libx264/FFmpeg software fallback depending on what the current encoder probe supports.
  * Do not show unavailable encoders as selectable.
  * If the current runtime already probes h264_amf / h264_nvenc / h264_qsv / libx264, wire the UI to that model.
* Add advanced FFmpeg/AMF options only in a controlled way.

  * Provide an advanced text/key-value area only if it can be safely passed to the encoder.
  * Validate or clearly warn that invalid options may make recording fail.
  * Do not let invalid custom options crash the app.
  * Log the final encoder/options actually used.
* Add capture border setting if supported.

  * Provide a “Show Windows capture border” toggle.
  * Default should be disabled if GraphicsCaptureSession.IsBorderRequired = false is supported.
  * If unsupported by SDK/runtime, hide the setting or show it as unsupported and document why.
* Do not add fake controls.

  * Every visible setting must either be wired, explicitly read-only, or clearly marked as future/unsupported.
  * Preserve existing replay and manual recording behavior.


Audio:

* Show current system audio capture status/settings only if wired.
* Do not claim microphone mixing is supported unless it actually is.
* If microphone is still not muxed into output, clearly keep it out of completed settings or mark it future work.

6. Apply settings correctly

* Saving from Settings must write the user config file.
* The recorder must actually use the saved settings.
* Output folder changes must affect future replay clips and future manual recordings.
* Apply changes live where safe.
* For settings that require restart, clearly mark them as restart-required.
* Do not present settings as live-applied if they are not.
* Avoid race conditions while recording:

  * changing recording paths during active recording should not corrupt the current recording;
  * path changes can apply to the next recording/clip if safer.
* Preserve existing working replay hotkey save behavior.
* Preserve existing working manual recording start/stop/pause/resume behavior.

7. Config/schema cleanup

* Update config/default-config.json so it matches the actual implemented settings.
* Remove or avoid placeholder settings that are not used.
* Keep names consistent and stable.
* Make sure Settings UI, recorder config loading, and default-config.json all agree.
* Do not create two separate incompatible config systems.

8. Branding cleanup

* User-facing branding must say Monolith, not WindowsRecorder.
* Check:

  * Settings window title
  * recorder tray tooltip/menu
  * executable metadata if reasonable
  * logs
  * config sample
  * docs
  * CI artifact names if reasonable
* Do not rename risky build targets unless needed and safe.
* The important fix is that the visible app/product should be Monolith and Monolith.Settings.exe should exist.

9. Build/CI cleanup

* Update local build instructions if needed.
* Update GitHub Actions workflow so Settings is built too.
* If CI artifact upload exists, include Monolith.Settings.exe and any required runtime files.
* Make sure the workflow does not only build app/recorder while skipping Settings.
* Keep build time reasonable; do not add unnecessary full rebuild steps.
* Do not hide build errors.

10. Documentation/status cleanup
    Update the project docs so they reflect reality:

* ACTIVE_HANDOVER.md
* ROADMAP.md
* ARCHITECTURE.md
* handoff.txt
* any setup/build docs

Docs must say:

* replay hotkey MKV saving works;
* manual recording works if still true after this change;
* Settings is implemented only if it actually opens, saves config, and affects runtime behavior;
* Stream Deck is still not implemented;
* microphone mixing is still not completed unless actually implemented;
* yellow capture border behavior is documented honestly;
* Monolith.Settings.exe is now part of the build/output.

Constraints:

* Do not touch Stream Deck implementation.
* Do not redesign replay buffer.
* Do not rewrite capture/encoding/manual recording unless required to connect settings.
* Do not add fake Settings controls.
* Do not hardcode user-specific paths.
* Do not break existing replay save.
* Do not break manual recording.
* Keep changes focused but complete enough that Settings is actually usable.
* Do not commit unless explicitly asked.

Verification:
Run the relevant build locally if possible.

Verify:

* main recorder builds;
* Monolith.Settings.exe is produced;
* Monolith.Settings.exe is present where the recorder expects it;
* opening Settings from the tray works;
* Settings window opens without error;
* settings are saved to AppData\Local\Monolith config;
* closing and reopening the app preserves settings;
* replay clip output folder setting is respected;
* manual recording output folder setting is respected;
* reset-to-default path behavior works;
* invalid config does not crash the app;
* Ctrl+Shift+F8 replay save still writes a valid MKV;
* Ctrl+Shift+F9/F10/F11 manual recording still works;
* user media does not default to AppData;
* yellow capture border is disabled if supported, or clearly reported if not supported.

Done when:

* Monolith.Settings.exe is built and included in output/artifacts.
* Settings opens from the app without missing-file errors.
* Settings persist across restarts.
* Implemented settings are actually wired to runtime behavior.
* Existing recording/replay features still work.
* Docs and handoff files are coherent.
* Final report lists:

  * root cause of missing Monolith.Settings.exe;
  * changed files;
  * build commands used;
  * build result;
  * settings implemented;
  * settings live-applied vs restart-required;
  * manual tests completed;
  * remaining risks or TODOs.
