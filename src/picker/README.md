# src/picker

`pipewire-waybar-picker` — GTK4 layer-shell popup that lists the configured
sinks and calls `SetChosen` on the daemon when one is selected. Designed to
be bound to the Waybar widget's `on-click`.

## Behaviour

- Anchored as a fullscreen `GTK_LAYER_SHELL_LAYER_OVERLAY` surface with a
  fully transparent background. The visible listbox sits in the top-right
  corner via `halign=END / valign=START` plus margins.
- Keyboard mode is `EXCLUSIVE`: the window grabs keyboard focus while shown,
  so Escape dismisses it.
- A `GtkGestureClick` on the root checks the click position against the
  listbox bounds; clicks outside dismiss the window. This is the standard
  wofi/fuzzel "modal popup" pattern — necessary because layer-shell windows
  don't get focus-loss events on Hyprland the way regular toplevels do.
- Each row shows the type icon (from config) and friendly name; the chosen
  row is marked, unavailable rows are dimmed and non-activatable.
- If the daemon is unreachable the listbox shows an error label instead of
  rows; the window is still dismissable.

## Files

- **`main.cpp`** — the whole picker. UI construction, D-Bus calls
  (`ListSinks`, `SetChosen`), event handlers.

## Why `G_APPLICATION_NON_UNIQUE`

Each invocation is its own GTK application instance. Without this flag,
clicking the Waybar widget while the picker is already open would
"reactivate" the existing instance instead of dismissing it. Combined with
click-outside-to-dismiss this gives the expected toggle behaviour.
