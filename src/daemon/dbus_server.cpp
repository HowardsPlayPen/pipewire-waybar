#include "daemon/dbus_server.h"

#include "common/log.h"
#include "pipewire-waybar/dbus_interface.h"

#include <stdexcept>
#include <tuple>
#include <vector>

namespace phc {

DbusServer::DbusServer(PwClient& pw, Router& router, std::function<void()> reload_cb)
    : pw_(pw), router_(router), reload_cb_(std::move(reload_cb)) {}

DbusServer::~DbusServer() {
    {
        std::lock_guard<std::mutex> lk(emit_mu_);
        emit_stop_ = true;
    }
    emit_cv_.notify_all();
    if (emit_thread_.joinable()) emit_thread_.join();
}

void DbusServer::start() {
    conn_ = sdbus::createSessionBusConnection(sdbus::ServiceName{dbus::kService});
    object_ = sdbus::createObject(*conn_, sdbus::ObjectPath{dbus::kObject});

    auto iface = sdbus::InterfaceName{dbus::kInterface};

    object_->addVTable(
        sdbus::registerMethod(dbus::kMethodListSinks)
            .implementedAs([this]() {
                std::vector<sdbus::Struct<std::string, std::string, std::string, bool, bool>> out;
                auto lk = pw_.lock();
                for (auto& s : router_.snapshot()) {
                    out.emplace_back(sdbus::Struct{s.name, s.type, s.node_name, s.available, s.chosen});
                }
                return out;
            }),
        sdbus::registerMethod(dbus::kMethodGetChosen)
            .implementedAs([this]() {
                auto lk = pw_.lock();
                return router_.chosen();
            }),
        sdbus::registerMethod(dbus::kMethodSetChosen)
            .implementedAs([this](const std::string& name) {
                bool ok;
                {
                    auto lk = pw_.lock();
                    ok = router_.choose(name);
                }
                if (!ok) {
                    throw sdbus::Error(sdbus::Error::Name{"org.pipewire_waybar.Sink1.Error.UnknownSink"},
                                       "unknown sink: " + name);
                }
            }),
        sdbus::registerMethod(dbus::kMethodReload)
            .implementedAs([this]() {
                if (reload_cb_) reload_cb_();
            }),
        sdbus::registerSignal(dbus::kSignalChosen).withParameters<std::string>(),
        sdbus::registerSignal(dbus::kSignalSinks),
        sdbus::registerSignal(dbus::kSignalAppeared).withParameters<std::string, std::string>()
    ).forInterface(iface);

    registered_ = true;
    log::info("dbus: listening as ", dbus::kService, " on session bus");

    // sdbus-c++ does not run its event loop automatically; start it now so
    // method calls and signals are dispatched on a background thread.
    conn_->enterEventLoopAsync();

    emit_thread_ = std::thread([this] { run_emit_loop_(); });
}

void DbusServer::run_emit_loop_() {
    std::unique_lock<std::mutex> lk(emit_mu_);
    while (true) {
        emit_cv_.wait(lk, [this] { return emit_stop_ || !emit_queue_.empty(); });
        while (!emit_queue_.empty()) {
            auto fn = std::move(emit_queue_.front());
            emit_queue_.pop_front();
            lk.unlock();
            try { fn(); } catch (const std::exception& e) {
                log::warn("dbus: emit failed: ", e.what());
            } catch (...) {
                log::warn("dbus: emit failed: unknown exception");
            }
            lk.lock();
        }
        if (emit_stop_) return;
    }
}

void DbusServer::enqueue_emit_(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(emit_mu_);
        if (emit_stop_) return;
        emit_queue_.push_back(std::move(fn));
    }
    emit_cv_.notify_one();
}

void DbusServer::emit_chosen_changed(const std::string& chosen) {
    if (!registered_) return;
    enqueue_emit_([this, chosen] {
        object_->emitSignal(sdbus::SignalName{dbus::kSignalChosen})
               .onInterface(sdbus::InterfaceName{dbus::kInterface})
               .withArguments(chosen);
    });
}

void DbusServer::emit_sinks_changed() {
    if (!registered_) return;
    enqueue_emit_([this] {
        object_->emitSignal(sdbus::SignalName{dbus::kSignalSinks})
               .onInterface(sdbus::InterfaceName{dbus::kInterface});
    });
}

void DbusServer::emit_device_appeared(const std::string& name, const std::string& type) {
    if (!registered_) return;
    enqueue_emit_([this, name, type] {
        object_->emitSignal(sdbus::SignalName{dbus::kSignalAppeared})
               .onInterface(sdbus::InterfaceName{dbus::kInterface})
               .withArguments(name, type);
    });
}

}  // namespace phc
