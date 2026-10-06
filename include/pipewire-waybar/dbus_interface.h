#pragma once

namespace phc::dbus {

inline constexpr const char* kService   = "org.pipewire_waybar";
inline constexpr const char* kObject    = "/org/pipewire_waybar";
inline constexpr const char* kInterface = "org.pipewire_waybar.Sink1";

// Methods
//   ListSinks  () -> a(sssbb)   array of (name, type, node_name, available, isChosen)
//   GetChosen  () -> s          friendly name; "" if none chosen
//   SetChosen  (s) -> ()        pick by friendly name
//   Reload     () -> ()         re-read config
//
// Signals
//   ChosenChanged  (s)          new friendly name
//   SinksChanged   ()           availability changed
//   DeviceAppeared (ss)         (name, type) - newly visible configured device
inline constexpr const char* kMethodListSinks  = "ListSinks";
inline constexpr const char* kMethodGetChosen  = "GetChosen";
inline constexpr const char* kMethodSetChosen  = "SetChosen";
inline constexpr const char* kMethodReload     = "Reload";
inline constexpr const char* kSignalChosen     = "ChosenChanged";
inline constexpr const char* kSignalSinks      = "SinksChanged";
inline constexpr const char* kSignalAppeared   = "DeviceAppeared";

}  // namespace phc::dbus
