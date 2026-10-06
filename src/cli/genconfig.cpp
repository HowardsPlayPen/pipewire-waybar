#include "cli/genconfig.h"

#include <nlohmann/json.hpp>
#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>
#include <spa/utils/hook.h>

#include <cstring>
#include <iostream>
#include <map>
#include <string>

namespace phc {

namespace {

struct DiscoveredSink {
    std::string node_name;
    std::string description;
    std::string nick;
    std::string type;
};

struct GenContext {
    pw_main_loop* loop{nullptr};
    pw_context* ctx{nullptr};
    pw_core* core{nullptr};
    pw_registry* registry{nullptr};
    spa_hook registry_listener{};
    spa_hook core_listener{};
    int sync_seq{0};
    std::map<uint32_t, DiscoveredSink> sinks;
};

const char* dlookup(const spa_dict* d, const char* k) {
    return d ? spa_dict_lookup(d, k) : nullptr;
}

bool dbool(const spa_dict* d, const char* k) {
    const char* v = dlookup(d, k);
    return v && (std::strcmp(v, "true") == 0 || std::strcmp(v, "1") == 0);
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string classify(const spa_dict* props) {
    const char* api = dlookup(props, "device.api");
    if (api && std::strcmp(api, "bluez5") == 0) return "bluetooth";

    const char* nm = dlookup(props, "node.name");
    std::string nn = nm ? nm : "";
    if (contains(nn, "iec958"))  return "spdif";
    if (contains(nn, "hdmi"))    return "hdmi";
    if (contains(nn, ".usb-") || contains(nn, "_usb-")) return "usb";

    if (api && std::strcmp(api, "alsa") == 0) return "analog";
    return "unknown";
}

extern "C" void reg_global(void* data, uint32_t id, uint32_t /*perm*/,
                           const char* type, uint32_t /*ver*/,
                           const spa_dict* props) {
    auto* gc = static_cast<GenContext*>(data);
    if (!type || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;

    const char* mc = dlookup(props, "media.class");
    if (!mc || std::strcmp(mc, "Audio/Sink") != 0) return;
    if (dbool(props, "node.virtual")) return;  // skip combine-stream etc.

    DiscoveredSink s;
    const char* nn = dlookup(props, "node.name");
    const char* nd = dlookup(props, "node.description");
    const char* nk = dlookup(props, "node.nick");
    s.node_name   = nn ? nn : "";
    s.description = nd ? nd : "";
    s.nick        = nk ? nk : "";
    s.type        = classify(props);
    if (s.node_name.empty()) return;
    gc->sinks[id] = std::move(s);
}

extern "C" void reg_global_remove(void*, uint32_t) {}

const struct pw_registry_events kRegEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = reg_global,
    .global_remove = reg_global_remove,
};

extern "C" void core_done(void* data, uint32_t id, int seq) {
    auto* gc = static_cast<GenContext*>(data);
    if (id == PW_ID_CORE && seq == gc->sync_seq) {
        pw_main_loop_quit(gc->loop);
    }
}

const struct pw_core_events kCoreEvents = {
    .version = PW_VERSION_CORE_EVENTS,
    .done = core_done,
};

std::string friendly_name(const DiscoveredSink& s) {
    if (!s.nick.empty())        return s.nick;
    if (!s.description.empty()) return s.description;
    return s.node_name;
}

// Default icon block, kept identical to config.example.json so the
// generated file styles correctly with Nerd Fonts out of the box.
nlohmann::json default_icons() {
    return nlohmann::json::parse(R"({
        "hdmi": "\udb83\udf62",
        "spdif": "\uf001",
        "usb": "\uf287",
        "bluetooth": "\uf293",
        "virtual": "\udb80\udcc3",
        "analog": "\uf028",
        "unknown": "\uf028"
    })");
}

}  // namespace

int generate_config(std::ostream& out) {
    pw_init(nullptr, nullptr);

    GenContext gc;
    gc.loop = pw_main_loop_new(nullptr);
    if (!gc.loop) {
        std::cerr << "pw_main_loop_new failed\n";
        return 1;
    }

    gc.ctx = pw_context_new(pw_main_loop_get_loop(gc.loop), nullptr, 0);
    if (!gc.ctx) {
        std::cerr << "pw_context_new failed\n";
        pw_main_loop_destroy(gc.loop);
        return 1;
    }

    gc.core = pw_context_connect(gc.ctx, nullptr, 0);
    if (!gc.core) {
        std::cerr << "Cannot connect to PipeWire (is it running?)\n";
        pw_context_destroy(gc.ctx);
        pw_main_loop_destroy(gc.loop);
        return 1;
    }

    gc.registry = pw_core_get_registry(gc.core, PW_VERSION_REGISTRY, 0);
    if (!gc.registry) {
        std::cerr << "pw_core_get_registry failed\n";
        pw_core_disconnect(gc.core);
        pw_context_destroy(gc.ctx);
        pw_main_loop_destroy(gc.loop);
        return 1;
    }

    spa_zero(gc.registry_listener);
    pw_registry_add_listener(gc.registry, &gc.registry_listener, &kRegEvents, &gc);
    spa_zero(gc.core_listener);
    pw_core_add_listener(gc.core, &gc.core_listener, &kCoreEvents, &gc);
    gc.sync_seq = pw_core_sync(gc.core, PW_ID_CORE, 0);

    // Run until the initial sync completes (registry fully enumerated).
    pw_main_loop_run(gc.loop);

    // Build JSON.
    nlohmann::json root;
    auto& js_sinks = root["sinks"] = nlohmann::json::array();
    std::string default_name;
    for (auto& [id, s] : gc.sinks) {
        nlohmann::json e;
        std::string friendly = friendly_name(s);
        if (default_name.empty()) default_name = friendly;
        e["name"]      = friendly;
        e["node_name"] = s.node_name;
        e["type"]      = s.type;
        js_sinks.push_back(std::move(e));
    }
    root["virtual_sinks"] = nlohmann::json::array();
    if (!default_name.empty()) root["default"] = default_name;
    root["icons"] = default_icons();

    // ensure_ascii=true so Nerd Font glyphs come out as \uXXXX escapes,
    // matching the example file format.
    out << root.dump(/*indent=*/4, /*indent_char=*/' ', /*ensure_ascii=*/true) << "\n";

    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(gc.registry));
    pw_core_disconnect(gc.core);
    pw_context_destroy(gc.ctx);
    pw_main_loop_destroy(gc.loop);
    pw_deinit();
    return 0;
}

}  // namespace phc
