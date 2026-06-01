# Startup Script Layout

Startup scripts are grouped by ownership so gameplay modes cannot accidentally
fall through into each other.

- `platformer/`: the Spine platformer mode, including its generated level and
  enemy data.
- `dungeon/`: the procedural dungeon mode.
- `editor/`: a minimal editor/import bootstrap with no demo asset load. When
  no command-line map is provided, the app loads `assets/maps/editor_empty.map`;
  when `--load-map` is provided, only that map is loaded.
- `sandbox/`: standalone inspection/demo scripts such as free camera, pistol,
  props, and the Spine probe.
- `common/`: scripts shared by all modes, currently the ImGui control panel.

The native startup loader runs the selected mode folder first, then `common/`,
then any legacy direct `.luau` files left in this directory. The default mode is
`platformer`. Use `--startup-mode platformer`, `--startup-mode dungeon`,
`--startup-mode editor`, or `--startup-mode sandbox` to choose explicitly.
