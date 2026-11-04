# KES Documentation

This folder documents the engine as it exists in this repository today.

The goal is to describe the current runtime, asset formats, and tool workflow in a way that stays close to the code. If a doc and the implementation ever disagree, the implementation wins.

## Documents

- [Asset and File Formats](file-formats.md)
- [Engine API](engine-api.md)
- [Rendering (PBR, shadows, settings)](rendering.md)
- [UI System (HTML/CSS, RmlUi)](ui-system.md)
- [Tools and Asset Pipeline](tools-and-pipeline.md)
- [Profiling (Tracy)](profiling.md)
- [Crash Reporting](crash-reporting.md)

## Quick Facts

- The default runtime content directory is `assets` via `ENGINE_CONTENT_DIRECTORY`.
- A project now defines a single `AssetRoot`. The editor scans that root recursively and identifies assets by extension.
- The runtime can load loose files and `.pak` archives from the content directory.
- The main runtime asset extensions are `.kmodel`, `.ktex`, `.ksound`, `.kanim`, `.kagraph`, `.mat` and `.fmapc` (compiled maps from the Forge editor).

## Scope

These docs focus on:

- runtime-facing engine APIs used by game code
- on-disk asset and project formats
- import and packaging workflow

They do not try to document every internal renderer, editor, or importer implementation detail.
