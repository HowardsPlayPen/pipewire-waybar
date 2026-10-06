# src/common

Shared, dependency-light building blocks used by all three binaries
(`pipewire-waybard`, `pipewire-waybar`, `pipewire-waybar-picker`). Built
into the static library `phc_common`.

## Files

- **`types.h`** — plain data: `SinkSpec` (friendly name + node_name + type +
  optional members for virtual sinks), `SinkSnapshot` (the per-sink row
  returned over D-Bus). No behaviour, just shapes.
- **`config.{h,cpp}`** — loads and validates `config.json` (sinks, virtual
  sinks, default, icons). Resolves the XDG config path. Throws on invalid
  input.
- **`state.{h,cpp}`** — small JSON-backed store at
  `$XDG_STATE_HOME/pipewire-waybar/state.json`. Persists the last-chosen
  sink across daemon restarts.
- **`log.{h,cpp}`** — minimal level-tagged stderr logger
  (`phc::log::info`, `debug`, `warn`, `error`).

## Constraints

- No PipeWire, GTK, or sdbus-c++ dependencies — those belong in the
  binaries that need them. Anything in `common` must build with
  `nlohmann_json` alone.
- Header includes use the `common/foo.h` form so callers can write
  `#include "common/config.h"` regardless of where they live in the source
  tree.
