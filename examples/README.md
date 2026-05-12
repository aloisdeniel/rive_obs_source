# Authoring a `.riv` that reacts to OBS

This plugin forwards OBS frontend events as **trigger inputs** to the active
state machine in your `.riv` file. To make your file react, you author
triggers in the Rive editor and then map OBS events to those triggers in the
plugin's properties panel.

## 1. Author triggers in Rive

In the [Rive editor](https://rive.app):

1. Open your file and select the artboard you want to use.
2. Open the **State Machine** you want to expose to OBS.
3. In the **Inputs** panel, click **+** and add an input with type **Trigger**.
4. Give the trigger a stable, lowercase name like `streaming_started` (the
   name will be referenced verbatim from OBS).
5. Wire the trigger to whatever transition or animation you want it to
   advance to.

Repeat for every OBS event you want to react to. A typical "Live"
indicator might have just two:

- `streaming_started` → transitions an off-air idle state to a pulsing
  "LIVE" badge.
- `streaming_stopped` → transitions back to idle.

## 2. Export the `.riv`

In Rive: **File → Export → Runtime (.riv)**. Save the file somewhere your
OBS profile can read it.

## 3. Wire it up in OBS

1. **Sources → + → Rive Source**.
2. Pick the file, artboard, and state machine.
3. Scroll to the event-mapping section. For each event you authored a
   trigger for, type the trigger name into the corresponding field. The
   field is free-form: anything you type here is looked up by name on the
   state machine.
4. Toggle **Reload when file changes on disk** if you're still iterating —
   each export from Rive will live-update the source.

## Tips

- **Stable names.** OBS persists the *names* you type — if you rename a
  trigger in Rive, update it in OBS too. The plugin will log a one-time
  warning when a configured name doesn't match anything on the active
  state machine.
- **Multiple instances.** Each OBS source owns its own state machine
  instance. If you add the same Rive source twice (for example, to use
  it on two scenes with different mappings) each instance keeps its own
  state.
- **Default artboard / state machine.** If you leave the dropdowns at the
  defaults, the plugin uses the file's default artboard and default state
  machine — useful if your file only contains one of each.

## Sample patterns

A few combinations that work well for streamer overlays:

| Want                                                       | Trigger names to author                       |
|------------------------------------------------------------|-----------------------------------------------|
| "LIVE" badge that fades in when streaming starts            | `streaming_started`, `streaming_stopped`      |
| Recording light that pulses while REC and pauses when paused| `recording_started`, `recording_paused`, `recording_unpaused`, `recording_stopped` |
| Animated scene name nameplate                              | `scene_changed`                                |
| "Going live in 3..2..1..." countdown                       | `streaming_starting` (fires on the pre-stream phase) |

The full list of forwarded events is in the top-level
[README](../README.md#supported-obs-events).
