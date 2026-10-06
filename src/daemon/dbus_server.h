#pragma once

#include "common/types.h"
#include "daemon/pw_client.h"
#include "daemon/router.h"

#include <sdbus-c++/sdbus-c++.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace phc {

class DbusServer {
public:
    // reload_cb runs on the DBus thread; it must be thread-safe (it should
    // lock the pw thread loop and mutate state).
    DbusServer(PwClient& pw, Router& router,
               std::function<void()> reload_cb);
    ~DbusServer();

    DbusServer(const DbusServer&) = delete;
    DbusServer& operator=(const DbusServer&) = delete;

    // Begin serving. Returns immediately; sdbus-c++ runs its own event loop
    // in the background. Throws on bus failure.
    void start();

    // Emit signals (call from any thread).
    void emit_chosen_changed(const std::string& chosen);
    void emit_sinks_changed();
    void emit_device_appeared(const std::string& name, const std::string& type);

private:
    PwClient& pw_;
    Router& router_;
    std::function<void()> reload_cb_;

    std::unique_ptr<sdbus::IConnection> conn_;
    std::unique_ptr<sdbus::IObject> object_;

    std::atomic<bool> registered_{false};

    // Signal emission runs on a dedicated worker thread that holds no other
    // locks. PW callbacks fire under the PW loop mutex, and emitting a signal
    // synchronously from there would deadlock against an inbound DBus method
    // call whose handler is blocked acquiring that same lock.
    std::thread emit_thread_;
    std::mutex emit_mu_;
    std::condition_variable emit_cv_;
    std::deque<std::function<void()>> emit_queue_;
    bool emit_stop_{false};

    void enqueue_emit_(std::function<void()> fn);
    void run_emit_loop_();
};

}  // namespace phc
