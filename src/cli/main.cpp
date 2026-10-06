#include "cli/genconfig.h"
#include "common/config.h"
#include "common/log.h"
#include "pipewire-waybar/dbus_interface.h"

#include <fstream>

#include <nlohmann/json.hpp>
#include <sdbus-c++/sdbus-c++.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

namespace dbus_id = phc::dbus;

using SinkRow = sdbus::Struct<std::string, std::string, std::string, bool, bool>;

std::unique_ptr<sdbus::IProxy> make_proxy() {
    auto conn = sdbus::createSessionBusConnection();
    auto proxy = sdbus::createProxy(std::move(conn),
        sdbus::ServiceName{dbus_id::kService},
        sdbus::ObjectPath{dbus_id::kObject});
    return proxy;
}

std::vector<SinkRow> list_sinks(sdbus::IProxy& proxy) {
    std::vector<SinkRow> rows;
    proxy.callMethod(dbus_id::kMethodListSinks)
         .onInterface(dbus_id::kInterface)
         .storeResultsTo(rows);
    return rows;
}

std::string get_chosen(sdbus::IProxy& proxy) {
    std::string out;
    proxy.callMethod(dbus_id::kMethodGetChosen)
         .onInterface(dbus_id::kInterface)
         .storeResultsTo(out);
    return out;
}

void set_chosen(sdbus::IProxy& proxy, const std::string& name) {
    proxy.callMethod(dbus_id::kMethodSetChosen)
         .onInterface(dbus_id::kInterface)
         .withArguments(name);
}

void reload(sdbus::IProxy& proxy) {
    proxy.callMethod(dbus_id::kMethodReload)
         .onInterface(dbus_id::kInterface);
}

// ---- waybar JSON output ----

struct WatchState {
    phc::Config cfg;
    bool flash{false};
};

nlohmann::json build_waybar_json(const WatchState& ws,
                                 const std::vector<SinkRow>& rows,
                                 const std::string& chosen) {
    auto icon_for = [&](const std::string& type) -> std::string {
        auto it = ws.cfg.icons.find(type);
        if (it != ws.cfg.icons.end()) return it->second;
        auto fallback = ws.cfg.icons.find("unknown");
        return fallback != ws.cfg.icons.end() ? fallback->second : "";
    };

    std::string text;
    std::string css_class = "unknown";
    bool unavailable = false;

    if (!chosen.empty()) {
        for (auto& r : rows) {
            const auto& [name, type, node_name, available, is_chosen] = r;
            if (name != chosen) continue;
            text = icon_for(type) + "  " + name;
            css_class = type;
            unavailable = !available;
            break;
        }
    }
    if (text.empty()) {
        text = icon_for("unknown") + "  (no sink)";
        css_class = "unknown";
        unavailable = true;
    }

    if (unavailable) css_class += " unavailable";
    if (ws.flash)    css_class += " flash";

    std::string tooltip = "Click to change sink\n";
    for (auto& r : rows) {
        const auto& [name, type, node_name, available, is_chosen] = r;
        tooltip += (is_chosen ? "* " : "  ");
        tooltip += icon_for(type);
        tooltip += " ";
        tooltip += name;
        if (!available) tooltip += " (unavailable)";
        tooltip += "\n";
    }
    if (!tooltip.empty() && tooltip.back() == '\n') tooltip.pop_back();

    nlohmann::json j;
    j["text"] = text;
    j["class"] = css_class;
    j["tooltip"] = tooltip;
    j["alt"] = css_class;
    return j;
}

void print_waybar(const WatchState& ws, const std::vector<SinkRow>& rows,
                  const std::string& chosen) {
    std::cout << build_waybar_json(ws, rows, chosen).dump() << std::endl;
}

// ---- subcommands ----

int cmd_list() {
    auto proxy = make_proxy();
    auto rows = list_sinks(*proxy);
    for (auto& r : rows) {
        const auto& [name, type, node_name, available, is_chosen] = r;
        std::cout << (is_chosen ? "* " : "  ")
                  << "[" << type << "] " << name
                  << (available ? "" : " (unavailable)")
                  << "  -- " << node_name << "\n";
    }
    return 0;
}

int cmd_get() {
    auto proxy = make_proxy();
    std::cout << get_chosen(*proxy) << "\n";
    return 0;
}

int cmd_set(const std::string& name) {
    auto proxy = make_proxy();
    try {
        set_chosen(*proxy, name);
    } catch (const sdbus::Error& e) {
        std::cerr << "set failed: " << e.getName() << ": " << e.getMessage() << "\n";
        return 1;
    }
    return 0;
}

int cmd_reload() {
    auto proxy = make_proxy();
    reload(*proxy);
    return 0;
}

std::atomic<bool> g_stop{false};
std::condition_variable g_cv;
std::mutex g_mu;

int cmd_watch(const std::filesystem::path& cfg_path) {
    WatchState ws;
    try { ws.cfg = phc::Config::load(cfg_path); }
    catch (const std::exception& e) {
        // Don't die — waybar will restart-loop us. Carry on with no
        // config so the bar still renders text from the daemon.
        std::cerr << "watch: config load failed (continuing without icons): "
                  << e.what() << "\n";
    }

    auto conn = sdbus::createSessionBusConnection();
    auto proxy = sdbus::createProxy(*conn,
        sdbus::ServiceName{dbus_id::kService},
        sdbus::ObjectPath{dbus_id::kObject});

    std::mutex emit_mu;
    auto refresh_and_emit = [&](bool flash_now = false) {
        std::lock_guard lk(emit_mu);
        try {
            auto rows = list_sinks(*proxy);
            auto chosen = get_chosen(*proxy);
            ws.flash = flash_now;
            print_waybar(ws, rows, chosen);
        } catch (const std::exception& e) {
            // Daemon unreachable — still print something so the bar isn't blank.
            nlohmann::json j;
            j["text"] = "  (no daemon)";
            j["class"] = "unavailable";
            j["tooltip"] = std::string("pipewire-waybard unreachable: ") + e.what();
            std::cout << j.dump() << std::endl;
        }
    };

    proxy->uponSignal(sdbus::SignalName{dbus_id::kSignalChosen})
         .onInterface(sdbus::InterfaceName{dbus_id::kInterface})
         .call([&](const std::string&) { refresh_and_emit(false); });

    proxy->uponSignal(sdbus::SignalName{dbus_id::kSignalSinks})
         .onInterface(sdbus::InterfaceName{dbus_id::kInterface})
         .call([&]() { refresh_and_emit(false); });

    proxy->uponSignal(sdbus::SignalName{dbus_id::kSignalAppeared})
         .onInterface(sdbus::InterfaceName{dbus_id::kInterface})
         .call([&](const std::string&, const std::string&) {
             refresh_and_emit(true);
             // Drop flash class after ~600ms by re-emitting non-flash state.
             std::thread([&]() {
                 std::this_thread::sleep_for(std::chrono::milliseconds(600));
                 refresh_and_emit(false);
             }).detach();
         });

    auto on_signal = [](int) { g_stop = true; g_cv.notify_all(); };
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    refresh_and_emit(false);  // initial state

    // sdbus-c++ does not auto-run the event loop; without this, signal
    // callbacks never fire.
    conn->enterEventLoopAsync();

    std::unique_lock lk(g_mu);
    g_cv.wait(lk, []{ return g_stop.load(); });
    return 0;
}

int cmd_generate_config(int argc, char** argv) {
    std::filesystem::path out_path;
    bool force = false;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--out" || a == "-o") && i + 1 < argc) out_path = argv[++i];
        else if (a == "--force" || a == "-f") force = true;
    }

    if (out_path.empty()) {
        return phc::generate_config(std::cout);
    }

    if (std::filesystem::exists(out_path) && !force) {
        std::cerr << "refusing to overwrite " << out_path
                  << " (pass --force to override)\n";
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(out_path.parent_path(), ec);
    std::ofstream f(out_path);
    if (!f) {
        std::cerr << "cannot open " << out_path << " for writing\n";
        return 1;
    }
    int rc = phc::generate_config(f);
    if (rc == 0) {
        std::cerr << "wrote baseline config to " << out_path << "\n";
    }
    return rc;
}

void usage() {
    std::cerr <<
        "Usage:\n"
        "  pipewire-waybar list\n"
        "  pipewire-waybar get\n"
        "  pipewire-waybar set <name>\n"
        "  pipewire-waybar reload\n"
        "  pipewire-waybar watch [--config PATH]\n"
        "  pipewire-waybar generate-config [--out PATH] [--force]\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    std::string sub = argv[1];

    try {
        if (sub == "list")   return cmd_list();
        if (sub == "get")    return cmd_get();
        if (sub == "reload") return cmd_reload();
        if (sub == "set") {
            if (argc < 3) { usage(); return 2; }
            return cmd_set(argv[2]);
        }
        if (sub == "watch") {
            std::filesystem::path cfg = phc::Config::default_path();
            for (int i = 2; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--config" && i + 1 < argc) cfg = argv[++i];
            }
            return cmd_watch(cfg);
        }
        if (sub == "generate-config" || sub == "gen-config") {
            return cmd_generate_config(argc - 2, argv + 2);
        }
        if (sub == "-h" || sub == "--help") { usage(); return 0; }
    } catch (const sdbus::Error& e) {
        std::cerr << "dbus error: " << e.getName() << ": " << e.getMessage() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    usage();
    return 2;
}
