# Authoring a `.riv` that reacts to OBS

The plugin drives your file through view-model binding: a `SceneViewModel`
filled from the current OBS scene every frame, and an optional JSON file
feeding a custom view model. See the top-level
[README](../README.md#scene-view-model-binding) for the property schema and
the JSON mapping rules.

## Sample files

- [`obssource.riv`](obssource.riv) — a small file that declares
  `SceneViewModel` and a `CustomDataViewModel`, ready to drop into OBS.
- [`obssource.json`](obssource.json) — the custom-data file that pairs with
  it. Point the **Custom data file (JSON)** field at this file to see the
  custom view model populate.

## Wire it up in OBS

1. **Sources → + → Rive Source**.
2. Pick the file, artboard, and state machine.
3. (Optional) Point **Custom data file (JSON)** at a `.json` file whose
   shape matches a view model in your `.riv`.

Both files are watched for changes — each export from Rive (or rewrite of
the JSON) live-updates the source.

## Tips

- **Default artboard / state machine.** If you leave the dropdowns at the
  defaults, the plugin uses the file's default artboard and default state
  machine — useful if your file only contains one of each.
- **Multiple instances.** Each OBS source owns its own state machine
  instance. If you add the same Rive source twice (for example, to use
  it on two scenes with different data files) each instance keeps its own
  state.
