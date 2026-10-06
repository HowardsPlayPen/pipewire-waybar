# src/daemon

The long-running `pipewire-waybard` process. Connects to PipeWire, watches
the graph, manages virtual sinks via `module-combine-stream`, enforces the
user's chosen sink, and exposes everything over D-Bus for the CLI and picker.

## Files

- **`main.cpp`** — wires the components together, parses `--config` /
  `--state` flags, installs SIGINT/SIGTERM handlers, owns the lifecycle.
- **`pw_client.{h,cpp}`** — thin wrapper over `libpipewire-0.3`. Owns the
  thread loop, registry, and metadata listener; tracks sinks and streams in
  `std::map<id, PwNode>`; exposes mutators (`set_default_sink`,
  `move_stream`, `load_module_combine_stream`, …) that callers must invoke
  under `pw.lock()`. All callbacks run on the PipeWire thread with the loop
  lock held.
- **`router.{h,cpp}`** — the policy layer. Knows about friendly names and
  virtual sinks, decides which node.name to target for a given choice,
  enforces the choice when streams appear or sinks (re)attach, fires
  `Notify` callbacks (`chosen_changed`, `sinks_changed`, `device_appeared`).
- **`combine_manager.{h,cpp}`** — owns `module-combine-stream` modules for
  virtual sinks. Loads/unloads modules to match the current config and
  reconciles on reload.
- **`dbus_server.{h,cpp}`** — sdbus-c++ vtable for the
  `org.pipewire_waybar.Sink1` interface (`ListSinks`, `GetChosen`,
  `SetChosen`, `Reload`) and signals (`ChosenChanged`, `SinksChanged`,
  `DeviceAppeared`). Translates D-Bus calls into router operations,
  taking `pw.lock()` first.
- **`config_watcher.{h,cpp}`** — inotify-based watcher that reloads the
  config when the file changes on disk. Watches the parent directory (so
  the write-tmp-then-rename pattern editors use is observed correctly) and
  debounces bursts to a single reload after ~150ms of quiet. Calls the same
  reload callback as the D-Bus `Reload` method, so editing the file is
  equivalent to invoking `pipewire-waybar reload`.

## Threading model

Two threads matter: the PipeWire thread loop and the D-Bus event thread.
`PwClient` is the synchronisation point — anyone touching tracked state or
calling mutators from outside the pw thread (i.e. from the D-Bus thread)
must hold `PwClient::Lock`. Router callbacks invoked from `PwClient` are
already on the pw thread with the lock held; they must not block on D-Bus.

## Adding a new D-Bus method

1. Declare the method name in `include/pipewire-waybar/dbus_interface.h`.
2. Wire the vtable entry in `dbus_server.cpp`, taking `pw.lock()` and
   delegating to `Router`.
3. Implement the operation on `Router`.
4. Add a CLI subcommand in `src/cli/main.cpp` if it should be scriptable.
