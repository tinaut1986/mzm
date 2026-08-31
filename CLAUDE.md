# Project notes for Claude

This is a fork of the Metroid: Zero Mission decompilation (`metroidret/mzm`) with
a 3DS port under `platform/3ds/` and tooling under `tools/`.

## Language: English in code

All **comments, function names, and variable names** must be written in English.
This applies to everything we own or add:

- `platform/3ds/` (the port)
- `tools/` (build scripts, the layer workbench, etc.)
- `docs/` and any new files

**Exception:** the upstream decompilation itself — `src/`, `include/`, `asm/`,
`data/`, `sound/`, and the other files that come from `metroidret/mzm`. Leave
those as they are; do not retranslate or churn them.

No project-wide sweep is wanted. Parts of `tools/layer-workbench/` (`build_maps.py`,
`mzmdata.py`, `serve.py`, `README.md`, `index.html`) are still in Spanish from an
earlier convention. Convert a file's comments/identifiers to English **as you
touch it** for other reasons, not as a standalone task. User-facing UI strings and
prose docs can stay as they are until their file is otherwise being reworked.
