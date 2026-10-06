#pragma once

#include <iosfwd>

namespace phc {

// Connect to PipeWire as a read-only client, enumerate Audio/Sink nodes,
// and write a baseline pipewire-waybar config.json to `out`.
//
// Returns 0 on success, non-zero on failure (PipeWire not running, etc.).
int generate_config(std::ostream& out);

}  // namespace phc
