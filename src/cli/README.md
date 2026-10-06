# src/cli

`pipewire-waybar` — the user-facing CLI plus the Waybar JSON feed.
Communicates with the daemon over D-Bus; doesn't talk to PipeWire directly.

## Subcommands

- **`list`** — print all configured sinks with type, node name, availability,
  and the current choice marker.
- **`get`** — print the currently chosen sink's friendly name.
- **`set <name>`** — change the chosen sink. Errors out if the daemon
  doesn't recognise the name.
- **`reload`** — tell the daemon to re-read its config file.
- **`watch`** — long-running JSON producer for Waybar's
  `custom/<...>` modules. Subscribes to daemon signals and emits a
  Waybar-shaped JSON object (`text`, `class`, `tooltip`, `alt`) on every
  change. Survives daemon restarts.
- **`generate-config [--out PATH] [--force]`** — introspect the current
  PipeWire graph and emit a starter `config.json`.

## Files

- **`main.cpp`** — subcommand dispatch, the `watch` loop (D-Bus signal
  subscriptions + sdbus-c++ async event loop), and Waybar JSON formatting.
- **`genconfig.{h,cpp}`** — standalone PipeWire introspection that produces
  a config skeleton from the live graph. Self-contained so it can run
  without a daemon.

## Notes for the `watch` command

Waybar restart-loops the producer if it exits, so transient errors (daemon
not yet running, dropped signals) just print a "(no daemon)" frame and
continue rather than aborting. SIGPIPE is ignored — Waybar may close stdout
when the bar reloads. The flash class on `DeviceAppeared` is dropped after
~600ms via a one-shot detached timer thread.
