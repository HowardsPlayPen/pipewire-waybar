#include "daemon/config_watcher.h"

#include "common/log.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>

namespace phc {

namespace fs = std::filesystem;

ConfigWatcher::ConfigWatcher(fs::path config_path, Callback cb)
    : path_(std::move(config_path)), cb_(std::move(cb)) {}

ConfigWatcher::~ConfigWatcher() { stop(); }

void ConfigWatcher::start() {
    if (thread_.joinable()) return;

    inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd_ < 0) {
        log::warn("config_watcher: inotify_init1 failed: ", std::strerror(errno));
        return;
    }
    event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd_ < 0) {
        log::warn("config_watcher: eventfd failed: ", std::strerror(errno));
        ::close(inotify_fd_);
        inotify_fd_ = -1;
        return;
    }
    stop_.store(false);
    thread_ = std::thread(&ConfigWatcher::run_, this);
}

void ConfigWatcher::stop() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    if (event_fd_ >= 0) {
        uint64_t one = 1;
        if (::write(event_fd_, &one, sizeof(one)) < 0) { /* best-effort wakeup */ }
    }
    thread_.join();
    if (inotify_fd_ >= 0) { ::close(inotify_fd_); inotify_fd_ = -1; }
    if (event_fd_ >= 0)   { ::close(event_fd_);   event_fd_   = -1; }
}

void ConfigWatcher::run_() {
    fs::path dir = path_.parent_path();
    if (dir.empty()) dir = ".";
    std::string fname = path_.filename().string();

    int wd = inotify_add_watch(inotify_fd_, dir.c_str(),
        IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (wd < 0) {
        log::warn("config_watcher: cannot watch ", dir.string(),
                  ": ", std::strerror(errno));
        return;
    }
    log::info("config_watcher: watching ", path_.string());

    using clock = std::chrono::steady_clock;
    constexpr auto debounce = std::chrono::milliseconds(150);
    std::optional<clock::time_point> pending;

    while (!stop_.load(std::memory_order_relaxed)) {
        pollfd fds[2] = {
            { inotify_fd_, POLLIN, 0 },
            { event_fd_,   POLLIN, 0 },
        };
        int timeout = -1;
        if (pending) {
            auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
                *pending - clock::now()).count();
            timeout = (delta > 0) ? static_cast<int>(delta) : 0;
        }

        int rc = ::poll(fds, 2, timeout);
        if (rc < 0) {
            if (errno == EINTR) continue;
            log::warn("config_watcher: poll error: ", std::strerror(errno));
            break;
        }
        if (fds[1].revents & POLLIN) break;  // stop fd signalled

        if (fds[0].revents & POLLIN) {
            alignas(inotify_event) char buf[4096];
            for (;;) {
                ssize_t n = ::read(inotify_fd_, buf, sizeof(buf));
                if (n <= 0) break;
                for (char* p = buf; p < buf + n; ) {
                    auto* ev = reinterpret_cast<inotify_event*>(p);
                    if (ev->len > 0 && fname == ev->name &&
                        (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE))) {
                        pending = clock::now() + debounce;
                    }
                    p += sizeof(inotify_event) + ev->len;
                }
            }
        }

        if (pending && clock::now() >= *pending) {
            pending.reset();
            try {
                cb_();
            } catch (const std::exception& e) {
                log::warn("config_watcher: reload callback threw: ", e.what());
            }
        }
    }

    ::inotify_rm_watch(inotify_fd_, wd);
}

}  // namespace phc
