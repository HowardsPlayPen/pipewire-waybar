#include "common/config.h"
#include "common/log.h"
#include "common/state.h"
#include "daemon/combine_manager.h"
#include "daemon/config_watcher.h"
#include "daemon/dbus_server.h"
#include "daemon/pw_client.h"
#include "daemon/router.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

std::atomic<bool> g_shutdown{false};
std::condition_variable g_cv;
std::mutex g_cv_mu;

void on_signal(int) {
    g_shutdown = true;
    g_cv.notify_all();
}

void install_signal_handlers() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
}

}  // namespace

int main(int argc, char** argv) {
    namespace fs = std::filesystem;

    fs::path config_path = phc::Config::default_path();
    fs::path state_path  = phc::StateStore::default_path();

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) { config_path = argv[++i]; }
        else if (a == "--state"  && i + 1 < argc) { state_path  = argv[++i]; }
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: pipewire-waybard [--config PATH] [--state PATH]\n";
            return 0;
        }
    }

    install_signal_handlers();

    phc::Config cfg;
    try {
        cfg = phc::Config::load(config_path);
    } catch (const std::exception& e) {
        phc::log::error("config: ", e.what());
        return 1;
    }

    phc::StateStore state(state_path);

    phc::PwClient pw;
    phc::CombineManager combiner(pw);
    phc::Router router(pw, cfg, combiner);

    // The reload implementation captures everything; runs on the dbus thread.
    auto reload_cb = [&]() {
        try {
            phc::Config new_cfg = phc::Config::load(config_path);
            auto lk = pw.lock();
            phc::Config old = cfg;
            cfg = std::move(new_cfg);
            combiner.reconcile(old, cfg);
            router.on_config_replaced(cfg);
        } catch (const std::exception& e) {
            phc::log::error("reload: ", e.what());
        }
    };

    phc::DbusServer dbus(pw, router, reload_cb);
    phc::ConfigWatcher cfg_watcher(config_path, reload_cb);

    router.set_notify({
        .chosen_changed = [&](const std::string& c) {
            state.set_chosen(c.empty() ? std::optional<std::string>{} : std::optional{c});
            dbus.emit_chosen_changed(c);
        },
        .sinks_changed = [&]() { dbus.emit_sinks_changed(); },
        .device_appeared = [&](const std::string& n, const std::string& t) {
            dbus.emit_device_appeared(n, t);
        },
    });

    pw.set_callbacks({
        .on_sink_added         = [&](const phc::PwNode& n)   { router.on_sink_added(n); },
        .on_sink_removed       = [&](uint32_t id)            { router.on_sink_removed(id); },
        .on_stream_added       = [&](const phc::PwNode& n)   { router.on_stream_added(n); },
        .on_stream_removed     = [&](uint32_t)               {},
        .on_stream_target_changed = [&](const phc::PwNode& n){ router.on_stream_target_changed(n); },
        .on_ready              = []() { phc::log::debug("pw: registry initial sync done"); },
        // Sinks can appear before the "default" metadata is bound, in which
        // case enforcement silently fails; redo it once metadata is writable.
        .on_metadata_bound     = [&]() { router.reapply(); },
    });

    try {
        pw.start();
    } catch (const std::exception& e) {
        phc::log::error("pw: ", e.what());
        return 1;
    }

    // Load combine modules and apply persisted choice (or config default).
    {
        auto lk = pw.lock();
        combiner.load_all(cfg);

        std::string initial;
        if (auto c = state.chosen(); c && cfg.find(*c)) initial = *c;
        else if (cfg.default_sink) initial = *cfg.default_sink;
        if (!initial.empty()) {
            router.set_initial_chosen(initial);
            // Don't emit chosen_changed yet — we want enforcement only.
            router.reapply();
        }
    }

    try {
        dbus.start();
    } catch (const std::exception& e) {
        phc::log::error("dbus: ", e.what());
        pw.stop();
        return 1;
    }

    cfg_watcher.start();

    phc::log::info("pipewire-waybard ready");

    // Block until signal.
    {
        std::unique_lock lk(g_cv_mu);
        g_cv.wait(lk, []{ return g_shutdown.load(); });
    }

    phc::log::info("shutting down");
    cfg_watcher.stop();
    pw.stop();
    return 0;
}
