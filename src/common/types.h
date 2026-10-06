#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace phc {

enum class SinkKind { Real, Virtual };

struct SinkSpec {
    std::string name;       // friendly name (config key)
    std::string type;       // hdmi|spdif|usb|bluetooth|virtual|...
    std::string node_name;  // PipeWire node.name to match (real sinks)
    SinkKind kind{SinkKind::Real};
    std::vector<std::string> members;  // for virtual sinks: friendly names of real sinks
};

struct SinkSnapshot {
    std::string name;
    std::string type;
    std::string node_name;
    bool available{false};
    bool chosen{false};
};

}  // namespace phc
