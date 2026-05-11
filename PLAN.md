# rive_obs_source — Plan

An OBS Studio plugin that adds a new video source rendering a `.riv` (Rive)
file. Rive's state machine is driven by OBS frontend events (streaming,
recording, scene transitions). Rendering uses the official Rive Renderer
through platform-native GPU APIs (Metal on macOS, D3D11 on Windows).

---

## 1. Requirements

### 1.1 Functional

- **New OBS source type** "Rive Source" registered via `obs_register_source_s`,
  flagged `OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW`.
- **Load a `.riv` file** from a user-chosen path; introspect its artboards and
  state machines.
- **Properties UI** exposes:
  - File picker (`.riv`).
  - Artboard dropdown (populated from file).
  - State machine dropdown (populated from selected artboard).
  - Output width / height (defaults to artboard intrinsic size).
  - Fit mode (`contain`, `cover`, `fill`, `fit-width`, `fit-height`, `none`,
    `scale-down`) and alignment (9-point grid).
  - Background clear color (RGBA, default transparent).
  - **Event mapping table**: for each supported OBS frontend event, a free-form
    text field naming the Rive state-machine **trigger input** to fire when
    that event occurs. Empty → ignored.
- **Render** the selected artboard + state machine into an OBS texture every
  frame at the OBS canvas's `video_info` framerate, advancing the state
  machine by the real delta.
- **Forward OBS frontend events** to the running state machine by firing the
  configured trigger input. Events covered in v1:
  - `STREAMING_STARTING`, `STREAMING_STARTED`, `STREAMING_STOPPING`,
    `STREAMING_STOPPED`
  - `RECORDING_STARTING`, `RECORDING_STARTED`, `RECORDING_STOPPING`,
    `RECORDING_STOPPED`, `RECORDING_PAUSED`, `RECORDING_UNPAUSED`
  - `SCENE_CHANGED`, `TRANSITION_STARTED`, `TRANSITION_STOPPED`
- **Persist** all settings via OBS's normal `obs_data_t` flow so scene
  collections survive restart.
- **Cleanly handle reload** when the file path, artboard, or SM changes
  without leaking GPU resources.

### 1.2 Non-functional

- Target **OBS Studio 30.x** (libobs API 30) on **macOS 12+** and **Windows
  10+** (x64; Apple Silicon + Intel on mac via universal binary).
- Plugin must not block the OBS graphics thread for more than ~1 ms per frame
  on a representative `.riv` at 1080p.
- No crashes on malformed `.riv`; surface errors in OBS log and source status.
- Zero audio in v1. Source is passive — no mouse/keyboard input forwarded.
- License: MIT (matches existing `LICENSE`). Bundle Rive's MIT notice.

### 1.3 Out of scope (v1)

- Linux build (kept architecturally feasible; not shipped).
- Rive audio playback.
- Interactive pointer/keyboard input.
- Receiving Rive Events *from* the file back into OBS (one-way for now;
  reserved for v2, see §5).
- Live data binding / view-models beyond trigger inputs.

---

## 2. Architecture

```
+--------------------+         +----------------------------+
|  OBS graphics      |         |  Rive runtime              |
|  thread            |         |  (C++ rive-runtime)        |
|                    |         |                            |
|  video_tick(dt) ---+-------->|  StateMachine::advance(dt) |
|                    |         |  Renderer::draw()          |
|  video_render() ---+----+    |     into shared GPU tex    |
|     samples shared |    |    +-------------+--------------+
|     texture via    |    |                  |
|     gs_effect      |    |   shared GPU texture
+--------------------+    |   (IOSurface on mac, NT handle on win)
                          v
                    +-----------+
                    | OBS gs_*  |
                    | texture   |
                    +-----------+

+----------------------------+
| obs_frontend event cb      |
|  -> lookup mapping ->      |
|     SM->getTrigger(name)   |
|       ->fire()             |
+----------------------------+
```

### 2.1 GPU interop

- **macOS**: Rive renders with Metal into an `MTLTexture` backed by an
  `IOSurface`. OBS uses libobs-opengl on macOS, so we wrap the same
  `IOSurface` in a GL texture via `CGLTexImageIOSurface2D` and hand it to
  libobs as a `gs_texture_t` (similar to how the mac-virtualcam / decklink
  plugins do interop).
- **Windows**: Rive renders with D3D11 into a texture created with
  `D3D11_RESOURCE_MISC_SHARED_NTHANDLE`. We open the NT handle inside the OBS
  D3D11 device and wrap it as a `gs_texture_t` via the libobs Win32 shared-
  texture API (`gs_texture_open_shared` / `gs_texture_open_nt_shared`).

### 2.2 Threading

- State machine `advance` and Rive submission happen on OBS's graphics thread
  inside `video_tick` so we share the OBS frame clock. Heavy GPU work runs on
  the Rive command buffer; we only block on a fence if the texture is needed
  the same frame for sampling.
- OBS frontend event callback runs on the UI thread → marshal into a thread-
  safe pending-triggers queue that `video_tick` drains. Avoids touching Rive
  objects from two threads.

### 2.3 Modules / files (proposed)

```
src/
  plugin-main.cpp          // obs_module_load, obs_register_source
  rive_source.{h,cpp}      // OBS source data + lifecycle + properties + render
  rive_file.{h,cpp}        // Wraps rive::File, lists artboards/SMs
  rive_renderer_mac.mm     // Metal backend + IOSurface bridge
  rive_renderer_win.cpp    // D3D11 backend + NT handle bridge
  rive_renderer.h          // Platform-abstract interface
  obs_events.{h,cpp}       // frontend event cb + trigger mapping
deps/
  rive-runtime/            // git submodule
cmake/                     // from obs-plugintemplate
.github/workflows/         // mac + win CI from obs-plugintemplate
```

---

## 3. Milestones

Each milestone ends in a runnable, demonstrable state.

### M1 — Skeleton plugin & cross-platform build (≈ 2 days)

- Fork `obsproject/obs-plugintemplate` into this repo, keep MIT license.
- CMake builds an empty plugin that registers a no-op source ("Rive Source")
  visible in OBS's Add Source menu.
- GitHub Actions: build on macOS (arm64+x64) and Windows (x64). Artifacts
  uploaded.
- **Exit criteria**: plugin loads in OBS 30 on both platforms; adding the
  source shows a blank rectangle.

### M2 — Vendor & build rive-runtime (≈ 2 days)

- Add `rive-runtime` as a git submodule under `deps/`.
- Wire its CMake into ours; build the C++ runtime + Rive Renderer with the
  Metal backend on mac and D3D11 backend on win, static linkage.
- Verify no symbol clashes with OBS's bundled deps.
- **Exit criteria**: plugin binary links cleanly with Rive on both platforms;
  size and load time still reasonable.

### M3 — File loading & introspection (≈ 2 days)

- Add `rive_file.cpp`: load bytes, call `rive::File::import`, enumerate
  artboards and state machines by name.
- Properties UI: file picker; artboard + SM dropdowns repopulate on file
  change via `obs_properties_add_list` + modified callback.
- Width/height/fit/alignment/bg color properties wired to `obs_data_t`.
- **Exit criteria**: selecting a `.riv` populates both dropdowns; settings
  round-trip through save/load.

### M4 — Offscreen render & GPU interop (≈ 5 days)

This is the highest-risk milestone. Build it platform-by-platform.

- **M4a (macOS)**: Metal device + command queue per source. Render selected
  artboard to an `MTLTexture` backed by `IOSurface`. Bridge to GL texture
  using `CGLTexImageIOSurface2D` on the OBS graphics thread. Sample that
  texture in `video_render` via a simple `gs_effect_t`.
- **M4b (Windows)**: D3D11 device per source (or share OBS's device if Rive
  supports it). Render to a shared NT-handle texture; open in OBS's device.
- Common: advance state machine in `video_tick` using `obs_get_video_info`
  for fps; respect width/height/fit/alignment.
- Handle source resize, device-lost, file replacement.
- **Exit criteria**: a complex `.riv` (with effects) plays inside an OBS
  scene at 60 fps on both platforms; no leaks across 30 minutes of churn
  (add/remove/replace file).

### M5 — OBS event forwarding (≈ 2 days)

- Subscribe via `obs_frontend_add_event_callback`.
- Event mapping table in properties (rendered as one text field per supported
  event, grouped by category). Each field names the Rive trigger input to
  fire. Empty means ignored.
- On event: enqueue `{trigger_name}` in a lock-protected queue; in
  `video_tick`, drain queue and call `SMI::asTrigger()->fire()` for each
  matching input on the active SM. Log a warning the first time a configured
  name doesn't match an input on the SM.
- For `SCENE_CHANGED` / `TRANSITION_STARTED`, optionally also expose a Rive
  **string** input (if present) carrying the scene name — gated by a "send
  scene name" toggle so users without a string input aren't forced to add
  one. (If string inputs are not supported by the runtime, drop this and
  document.)
- **Exit criteria**: a sample `.riv` with triggers named `streaming_started`,
  `recording_stopped`, etc., visibly reacts to those OBS actions.

### M6 — Polish, packaging, docs (≈ 3 days)

- Robust error reporting in OBS log + a status string property.
- Hot reload when the file on disk changes (optional toggle).
- Sample `.riv` shipped in `examples/` plus a 1-page user README explaining
  how to author a file that reacts to events.
- Packaging: macOS `.pkg` (notarized + stapled, signed with developer ID
  cert), Windows installer (NSIS from plugin template) and portable zip.
- Tag `v0.1.0`, attach artifacts to GitHub Release.
- **Exit criteria**: a clean install on a fresh machine works end-to-end
  without manual library juggling.

---

## 4. Risks

1. **Metal → OpenGL interop on macOS**: libobs is still GL-based on mac.
   IOSurface bridging is well-trodden but fiddly (color space, premultiplied
   alpha, Y-flip). Plan a 1-day spike inside M4a; if blocked, fall back to a
   CPU readback path as a temporary workaround (correctness over speed).
2. **rive-runtime build surface**: its CMake assumes certain toolchains; we
   may need to patch or carry a thin CMake shim. Submodule lets us pin.
3. **State machine API drift**: Rive's input API has evolved. Pin a known-
   good runtime commit and document the bump policy.
4. **Frontend events on plugin load**: callbacks may fire before our source
   exists; per-source registration must subscribe in `create` and
   unsubscribe in `destroy`, not at module load.
5. **Multiple instances of the source**: each needs its own Rive device /
   texture; verify no shared global state in our wrappers.

---

## 5. Open questions

1. **"Rive Events" wording**: in the Rive runtime, "Events" are messages the
   file emits *outward*. OBS-side events are inputs *into* the file, which
   in Rive terms map to **trigger inputs** on a state machine. The plan
   above treats the chosen option as "fire named trigger inputs on the
   state machine," which is the only mechanism that flows OBS → Rive.
   Please confirm — or, if you want bidirectional (Rive can also raise
   events that OBS reacts to, e.g. switch scene), we'll add that to v2.
2. **Should the source replace the OBS Browser source for animated
   overlays**, i.e. is this expected to be the primary motion-graphic
   source for a streamer's workflow? That affects how much we invest in
   polish (status, error UI, hot reload) vs. raw functionality.
3. **Signing & notarization on macOS**: do you already have an Apple
   Developer ID? Required for a frictionless install.
4. **Distribution channel**: GitHub Releases only, or also OBS's plugin
   directory listing?
5. **Frame-rate coupling**: should the Rive state machine advance at the
   OBS canvas fps (current plan) or at a fixed 60 Hz with interpolation?
   The former is simpler and matches what most OBS sources do.

---

## 6. Rough timeline

| Milestone | Effort | Notes                                          |
|-----------|--------|------------------------------------------------|
| M1        | 2 d    | Mostly template plumbing                       |
| M2        | 2 d    | Submodule + CMake glue                         |
| M3        | 2 d    | UI plumbing                                    |
| M4        | 5 d    | Highest risk; platform interop                 |
| M5        | 2 d    | Event mapping + UI                             |
| M6        | 3 d    | Packaging, docs, sample                        |
| **Total** | **~16 working days** | Excludes review cycles and v2 work |
