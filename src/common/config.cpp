#include "common/config.h"

#include "common/log.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace phc {

namespace fs = std::filesystem;
using nlohmann::json;

const SinkSpec* Config::find(std::string_view name) const {
    for (auto& s : sinks)
        if (s.name == name) return &s;
    return nullptr;
}

fs::path Config::default_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return fs::path(xdg) / "pipewire-waybar" / "config.json";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path(home) / ".config" / "pipewire-waybar" / "config.json";
    }
    return fs::path("/etc/pipewire-waybar/config.json");
}

namespace {

std::string get_str(const json& j, const char* key, const char* ctx) {
    if (!j.contains(key) || !j.at(key).is_string()) {
        std::ostringstream oss;
        oss << ctx << ": missing or non-string field '" << key << "'";
        throw std::runtime_error(oss.str());
    }
    return j.at(key).get<std::string>();
}

std::string get_str_or(const json& j, const char* key, std::string fallback) {
    if (j.contains(key) && j.at(key).is_string())
        return j.at(key).get<std::string>();
    return fallback;
}

}  // namespace

Config Config::load(const fs::path& path) {
    Config c;
    c.source_path = path;

    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config: " + path.string());

    json root;
    try {
        in >> root;
    } catch (const json::parse_error& e) {
        throw std::runtime_error(std::string("config parse error: ") + e.what());
    }

    if (!root.is_object())
        throw std::runtime_error("config root must be a JSON object");

    std::set<std::string> seen_names;

    if (root.contains("sinks")) {
        if (!root["sinks"].is_array())
            throw std::runtime_error("'sinks' must be an array");
        for (auto& js : root["sinks"]) {
            SinkSpec s;
            s.kind = SinkKind::Real;
            s.name = get_str(js, "name", "sinks[]");
            s.node_name = get_str(js, "node_name", "sinks[]");
            s.type = get_str_or(js, "type", "unknown");
            if (!seen_names.insert(s.name).second)
                throw std::runtime_error("duplicate sink name: " + s.name);
            c.sinks.push_back(std::move(s));
        }
    }

    if (root.contains("virtual_sinks")) {
        if (!root["virtual_sinks"].is_array())
            throw std::runtime_error("'virtual_sinks' must be an array");
        for (auto& jv : root["virtual_sinks"]) {
            SinkSpec s;
            s.kind = SinkKind::Virtual;
            s.name = get_str(jv, "name", "virtual_sinks[]");
            s.type = get_str_or(jv, "type", "virtual");
            if (!jv.contains("members") || !jv["members"].is_array())
                throw std::runtime_error("virtual sink '" + s.name + "' missing members array");
            for (auto& m : jv["members"]) {
                if (!m.is_string())
                    throw std::runtime_error("virtual sink '" + s.name + "' has non-string member");
                s.members.push_back(m.get<std::string>());
            }
            if (s.members.empty())
                throw std::runtime_error("virtual sink '" + s.name + "' has no members");
            if (!seen_names.insert(s.name).second)
                throw std::runtime_error("duplicate sink name: " + s.name);
            c.sinks.push_back(std::move(s));
        }
    }

    // Validate virtual sink members reference real sinks (and not other virtuals).
    for (auto& v : c.sinks) {
        if (v.kind != SinkKind::Virtual) continue;
        for (auto& m : v.members) {
            const SinkSpec* ref = c.find(m);
            if (!ref)
                throw std::runtime_error("virtual sink '" + v.name + "' references unknown member '" + m + "'");
            if (ref->kind != SinkKind::Real)
                throw std::runtime_error("virtual sink '" + v.name + "' member '" + m + "' is not a real sink");
        }
    }

    if (root.contains("default") && root["default"].is_string()) {
        c.default_sink = root["default"].get<std::string>();
        if (!c.find(*c.default_sink))
            throw std::runtime_error("'default' refers to unknown sink: " + *c.default_sink);
    }

    if (root.contains("icons") && root["icons"].is_object()) {
        for (auto it = root["icons"].begin(); it != root["icons"].end(); ++it) {
            if (it.value().is_string())
                c.icons[it.key()] = it.value().get<std::string>();
        }
    }

    log::info("loaded config from ", path.string(),
              " (", c.sinks.size(), " sinks total)");
    return c;
}

}  // namespace phc
