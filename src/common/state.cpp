#include "common/state.h"

#include "common/log.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <system_error>

namespace phc {

namespace fs = std::filesystem;
using nlohmann::json;

fs::path StateStore::default_path() {
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg)
        return fs::path(xdg) / "pipewire-waybar" / "state.json";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".local" / "state" / "pipewire-waybar" / "state.json";
    return fs::path("/var/lib/pipewire-waybar/state.json");
}

StateStore::StateStore(fs::path path) : path_(std::move(path)) {
    load();
}

void StateStore::load() {
    std::error_code ec;
    if (!fs::exists(path_, ec)) return;
    std::ifstream in(path_);
    if (!in) {
        log::warn("state: cannot open ", path_.string(), " (ignoring)");
        return;
    }
    try {
        json root;
        in >> root;
        if (root.is_object() && root.contains("chosen") && root["chosen"].is_string())
            chosen_ = root["chosen"].get<std::string>();
    } catch (const std::exception& e) {
        log::warn("state: failed to parse ", path_.string(), ": ", e.what());
    }
}

void StateStore::set_chosen(std::optional<std::string> value) {
    if (value == chosen_) return;
    chosen_ = std::move(value);
    save();
}

void StateStore::save() {
    std::error_code ec;
    fs::create_directories(path_.parent_path(), ec);
    auto tmp = path_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            log::error("state: cannot write ", tmp.string());
            return;
        }
        json root = json::object();
        if (chosen_) root["chosen"] = *chosen_;
        out << root.dump(2) << "\n";
    }
    fs::rename(tmp, path_, ec);
    if (ec) {
        log::error("state: rename failed: ", ec.message());
        fs::remove(tmp, ec);
    }
}

}  // namespace phc
