#pragma once

#include "common/types.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace phc {

struct Config {
    std::vector<SinkSpec> sinks;          // includes both real and virtual entries
    std::optional<std::string> default_sink;
    std::map<std::string, std::string> icons;  // type -> glyph

    std::filesystem::path source_path;

    // Resolve by friendly name. Returns nullptr if not found.
    const SinkSpec* find(std::string_view name) const;

    // Default config path: $XDG_CONFIG_HOME/pipewire-waybar/config.json
    // (or ~/.config/pipewire-waybar/config.json).
    static std::filesystem::path default_path();

    // Load from path. Throws std::runtime_error on parse/validation errors.
    static Config load(const std::filesystem::path& path);
};

}  // namespace phc
