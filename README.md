# rive_obs_source

> ⚠️ **Prototype — mostly AI-generated.** Most of this codebase was produced by
> an AI coding assistant under human direction. It is published as a working
> proof-of-concept, not a production-ready plugin: expect rough edges, missing
> error handling, and behaviour changes between versions. Review the code
> before using it in a streaming setup you care about, and please report bugs
> rather than assuming they're known.

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

## Scene view-model binding

If your `.riv` declares a view model named **`SceneViewModel`**, the plugin
binds an instance of it to the active state machine and refreshes its values
every frame from the current OBS scene. This lets data-bound text, shapes, and
nested artboards reflect what's on stream without writing a single trigger.

The binding is forgiving — every property below is optional. Declare only the
fields you actually need; the plugin silently skips the ones you omit.

### Root `SceneViewModel`

| Property name      | Type    | Value                                          |
|--------------------|---------|------------------------------------------------|
| `name`             | string  | Current scene name.                            |
| `width`            | number  | OBS canvas width, in pixels.                   |
| `height`           | number  | OBS canvas height, in pixels.                  |
| `streaming`        | boolean | True while streaming.                          |
| `recording`        | boolean | True while recording.                          |
| `recordingPaused`  | boolean | True while the recording is paused.            |
| `sourceCount`      | number  | Number of items in `sources`.                  |
| `audioCount`       | number  | Number of items in `audioSources`.             |
| `sources`          | list    | One entry per scene item. See below.           |
| `audioSources`     | list    | One entry per global audio input. See below.   |

### Inner view model for `sources`

The plugin discovers the inner view model used by the list automatically — pick
whatever name you like in the Rive editor (e.g. `SourceViewModel`), reference
it from the `sources` list's *type* field, and declare any subset of these
properties:

| Property name | Type    | Value                                                   |
|---------------|---------|---------------------------------------------------------|
| `name`        | string  | Source name as shown in the OBS dock.                   |
| `x`, `y`      | number  | Top-left of the source's AABB, in canvas pixels.        |
| `width`       | number  | AABB width in canvas pixels (accounts for scale/rotation). |
| `height`      | number  | AABB height in canvas pixels.                           |
| `visible`     | boolean | Visibility flag of the scene item.                      |

### Inner view model for `audioSources`

Same convention — any name, any subset of fields:

| Property name | Type    | Value                                                   |
|---------------|---------|---------------------------------------------------------|
| `name`        | string  | Audio input name.                                       |
| `volume`      | number  | Linear volume, `0.0` – `1.0` (matches OBS's slider).    |
| `muted`       | boolean | Mute flag.                                              |

The `audioSources` list mirrors the **Audio Mixer** dock — only top-level
audio inputs are included, not audio that's transitively in a scene.

### Notes

- `SceneViewModel` is **optional**. Files that don't declare one work exactly
  as before; the plugin no-ops the scene-state push.
- The lists are kept in sync with OBS each frame: items are added/removed
  as scene items appear or disappear. Don't pre-populate them in the editor.
- A new view-model instance is bound every time you reload the file or change
  the artboard / state machine.

## Custom data binding (JSON)

For data that isn't already in OBS — current time, the song you're playing,
the latest chat messages, anything else — point the **Custom data file (JSON)**
field at a `.json` file. The plugin polls it once per second and pushes the
parsed values into a view model in your `.riv` whenever the file changes on
disk. Pair it with anything that can write JSON (a script, a small bot, a
shell loop running `date > now.json`).

How the JSON maps onto your file depends on whether you're also using
`SceneViewModel`:

- **No `SceneViewModel`**: the plugin instantiates a view model and binds it
  to the state machine. JSON keys map directly to its property names.
  ```json
  { 
    "id": "root",
    "type": "OverlayVM",
    "time": "12:34:56",
    "song": "Daft Punk - Around the World",
    "chat": [
      { "type": "ChatMessage", "id": "msg-41", "user": "alice", "text": "hi"  },
      { "type": "ChatMessage", "id": "msg-42", "user": "bob",   "text": "yo"  }
    ] }
  ```
  ...maps onto a view model called `OverlayVM` with properties `time`
  (string), `song` (string), and `chat` (list of `ChatMessage` view models
  with `user` and `text` strings). The `"id"` field on each chat message is
  optional; see [Reserved keys](#reserved-keys-type-and-id) for what it does.

- **With `SceneViewModel`**: the OBS scene state stays bound as before; custom
  data is applied against the same root, so JSON keys must target nested view
  models you've added (the documented `SceneViewModel` properties are
  reserved for OBS). Add a property `data` of type `MyDataVM` and shape the
  JSON as `{ "data": { "type": "MyDataVM", "time": "...", ... } }`.

### Reserved keys: `"type"` and `"id"`

Two JSON keys are reserved as binding metadata and never written to view-model
properties (so don't name a Rive property `"type"` or `"id"`).

#### `"type"`

Names the view-model type for the surrounding object. It can appear on the
root, on any nested-object value, and on every list item. The plugin uses it
to:

- **Pick the root VM** when the artboard has no default view model — common
  when your file declares several VMs and you want to choose one explicitly.
- **Grow lists from empty.** Without `"type"`, the plugin discovers a list's
  inner VM by inspecting an existing item; an editor-empty list with no
  snapshot items therefore can't grow. Adding `"type"` to each item removes
  that requirement.
- **Self-document the data.** On nested-VM properties the parent's schema
  already pins the type, but the field is still allowed (and ignored) so
  your JSON files stay readable on their own.

#### `"id"`

A stable identifier the plugin uses to preserve view-model **instances**
across snapshots. When the same id reappears, the existing instance is kept
and only its property values are rewritten — so any state machine on that
instance, any animation in flight, and any internal counters survive. When the
id changes (or is removed), the instance is replaced or dropped:

- **List items** are diffed by id. Items whose id is in both the old and new
  snapshot keep their instance. New ids spawn fresh instances; ids that drop
  out are removed. Items without an `"id"` fall back to position-based
  matching, so files that don't use ids behave exactly as they did before.
- **Nested view-model properties**: a different id triggers
  `replaceViewModel()` on the parent, swapping the property's instance for a
  fresh one. Same id (or no id) → mutate in place.
- **The root**: a different id replaces the root VM and re-binds it to the
  state machine. Heavy — most authors won't need this; it only applies when
  no `SceneViewModel` is in use (the OBS scene root is owned by the scene
  binding and isn't replaced).

Use ids on objects whose identity matters (a chat message that should slide
out gracefully, a song that should crossfade rather than restart). Skip them
on objects you treat as a value bag (a `now` block whose `time` field updates
every second).

##### Worked example: a chat list

Suppose your overlay shows a chat feed and you want each new message to slide
in, sit there for a few seconds, then slide out — driven by a state machine
on the `ChatMessage` view model. Snapshot at `t=0`:

```json
{ "chat": [
    { "id": "msg-41", "user": "alice", "text": "hi"  },
    { "id": "msg-42", "user": "bob",   "text": "yo"  }
] }
```

A second later, alice's message scrolled off and a new one came in:

```json
{ "chat": [
    { "id": "msg-42", "user": "bob",     "text": "yo"     },
    { "id": "msg-43", "user": "carol",   "text": "hello!" }
] }
```

The plugin sees `msg-42` in both snapshots and keeps that view-model instance
intact — its slide-in animation has already finished, so it stays visible.
`msg-41` disappears, so its instance is removed (its slide-out, if any, runs
before destruction). `msg-43` is brand new, so a fresh `ChatMessage` instance
is created and its slide-in animation plays from the start.

Without ids the plugin would re-create both list items every snapshot, so
every message would appear to slide in again every second.

### Image properties

When the JSON value is a string and the matching Rive property is an **image**
(an asset image input on the view model), the plugin treats the string as a
URL and resolves it to a decoded `RenderImage`:

| Prefix       | Source                                                          |
|--------------|-----------------------------------------------------------------|
| `https://…`  | HTTPS GET via the platform's native HTTP client (NSURLSession on macOS, WinHTTP on Windows). |
| `http://…`   | Same; plain HTTP. May be blocked by macOS App Transport Security in some host applications. |
| `file://…`   | Local file read. Accepts `file:///absolute/path` and `file://localhost/absolute/path`. |
| empty string | Clears the image property.                                      |

```json
{ "type": "OverlayVM",
  "avatar":   "https://example.com/avatars/alice.png",
  "logo":     "file:///Users/me/Pictures/logo.png",
  "song_art": "" }
```

Fetches run on a background worker thread, so the graphics thread never
blocks on I/O. Decoding into GPU resources happens on the graphics thread the
next time the plugin ticks (typically the next frame). That means a freshly
seen URL appears one frame after it loads — fine for overlays, fine for chat
avatars, not appropriate if you need pixel-perfect frame timing on a
just-pointed-at image.

URLs are cached for the lifetime of the loaded `.riv`; pointing the same
property at the same URL is free. Reloading the `.riv` (manually or via
hot-reload) drops the cache. Failed fetches log a warning and are not
retried until the file reloads.

Notes:

- Supported decoders are whatever the Rive runtime's `decodeImage()` accepts —
  in practice PNG and JPEG.
- Image properties are scalar values. Diff-by-`"id"` doesn't apply to them;
  they're just "the URL the property currently points at."
- Don't use `data:` URIs or relative paths — the plugin doesn't try to
  resolve them.

### Value mapping rules

| JSON key / value | Behaviour                                                                       |
|------------------|---------------------------------------------------------------------------------|
| string           | sets a `string` property, or resolves a URL/path to an image (see above)        |
| number           | sets a `number` property                                                        |
| boolean          | sets a `boolean` property                                                       |
| object           | recurses into a nested view model                                               |
| array            | reconciles a `list` of view models (resized + diffed by `"id"`; recurses per item) |
| null / unknown   | ignored (property keeps its current value)                                      |
| `"type"`         | reserved metadata: names the view-model type; never written to the VM           |
| `"id"`           | reserved metadata: stable identity for diffing; never written to the VM         |

JSON keys with no matching property on the view model are silently ignored —
declare only the fields you care about.

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
