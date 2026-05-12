# rive_obs_source

An OBS Studio plugin that renders a [Rive](https://rive.app) `.riv` file as a
native video source. The file's state machine is driven by OBS frontend events
(streaming, recording, scene transitions), letting you build motion graphics
that react to your stream in real time.

- macOS 12+ (universal: Apple Silicon + Intel) and Windows 10+ (x64).
- OBS Studio 30.x.
- GPU rendering via the official Rive Renderer (Metal on macOS, D3D11 on
  Windows).

---

## Install

Download the latest release from the
[Releases](https://github.com/aloisdeniel/rive_obs_source/releases) page and
install:

- **macOS** — open the `.pkg` and follow the installer.
- **Windows** — run the installer `.exe`, or extract the portable `.zip` into
  `%ProgramFiles%\obs-studio\obs-plugins\64bit`.

Restart OBS Studio. "Rive Source" should appear in the **+** menu under
**Sources**.

## Use it

1. **Add → Rive Source**, name it.
2. Click **Browse...** next to *Rive file* and pick a `.riv`.
3. Pick an **Artboard** and a **State machine** from the dropdowns. They
   populate from whatever your file contains.
4. Set **Output width / height** (defaults to the artboard's intrinsic size),
   plus **Fit** and **Alignment** like any other OBS source.
5. Scroll to the **event mapping** section and type a trigger name into each
   OBS event you want the file to react to. Leave a field empty to ignore
   that event. See [examples/README.md](examples/README.md) for the trigger
   naming convention.

The **Status** line at the top of the properties panel shows the current load
state and the last error, if any. Click **Refresh status** after fixing
something on disk to re-display.

### Hot reload

Tick **Reload when file changes on disk** to have the source re-import the
`.riv` whenever its mtime changes. Handy while you're authoring: export
from Rive, switch to OBS, see the result.

## Supported OBS events

The events below are forwarded to the active state machine as **trigger
inputs**. Add a trigger with one of these names (or any name you map in the
properties panel) and the file will react when OBS fires the corresponding
event.

| OBS event             | Suggested trigger name   |
|-----------------------|--------------------------|
| Streaming starting    | `streaming_starting`     |
| Streaming started     | `streaming_started`      |
| Streaming stopping    | `streaming_stopping`     |
| Streaming stopped     | `streaming_stopped`      |
| Recording starting    | `recording_starting`     |
| Recording started     | `recording_started`      |
| Recording stopping    | `recording_stopping`     |
| Recording stopped     | `recording_stopped`      |
| Recording paused      | `recording_paused`       |
| Recording unpaused    | `recording_unpaused`     |
| Scene changed         | `scene_changed`          |
| Transition started    | `transition_started`     |
| Transition stopped    | `transition_stopped`     |

The names are arbitrary — they only need to match a trigger input on the
artboard's active state machine.

## Build from source

```sh
git clone --recurse-submodules https://github.com/aloisdeniel/rive_obs_source
cd rive_obs_source
cmake --preset macos       # or windows-x64
cmake --build build_macos --config RelWithDebInfo
```

The Rive runtime is vendored as a git submodule under `deps/`. Make sure
you cloned with `--recurse-submodules` (or run `git submodule update --init
--recursive` after cloning).

## License

MIT. See [LICENSE](LICENSE). Bundles the
[rive-runtime](https://github.com/rive-app/rive-runtime) (MIT) at link time.
