#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <thread>

namespace phc {

// Watches a single config file for changes and invokes a callback when it's
// modified. Uses inotify on the parent directory so that the
// write-tmp-then-rename pattern used by most editors is observed correctly
// (a watch on the file itself goes stale across rename(2)).
//
// Bursts of events within ~150ms collapse into a single callback.
class ConfigWatcher {
public:
    using Callback = std::function<void()>;

    ConfigWatcher(std::filesystem::path config_path, Callback cb);
    ~ConfigWatcher();

    ConfigWatcher(const ConfigWatcher&) = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;

    // Spawns the watcher thread. Best-effort: logs and returns silently if
    // inotify or the directory watch can't be set up — Reload via D-Bus
    // remains available.
    void start();

    // Joins the watcher thread.
    void stop();

private:
    void run_();

    std::filesystem::path path_;
    Callback cb_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    int inotify_fd_{-1};
    int event_fd_{-1};
};

}  // namespace phc
