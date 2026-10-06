#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_registry;
struct pw_metadata;
struct pw_proxy;
struct pw_impl_module;
struct spa_dict;
struct spa_hook_list;

namespace phc {

// Tracked PipeWire objects.

struct PwNode {
    uint32_t id{0};
    uint32_t serial{0};
    std::string node_name;       // node.name
    std::string description;     // node.description
    std::string media_class;     // Audio/Sink, Stream/Output/Audio, ...
    std::string app_name;        // application.name (streams)
    std::string target_object;   // current target.object metadata (streams)
    bool is_virtual{false};      // node.virtual — set on graph-internal nodes
                                 // (module-combine-stream's own streams)
};

struct PwModule {
    std::string tag;                 // user-supplied identifier (e.g. virtual sink name)
    pw_impl_module* mod{nullptr};    // destroy with pw_impl_module_destroy
};

class PwClient {
public:
    struct Callbacks {
        std::function<void(const PwNode&)> on_sink_added;
        std::function<void(uint32_t /*id*/)> on_sink_removed;
        std::function<void(const PwNode&)> on_stream_added;
        std::function<void(uint32_t /*id*/)> on_stream_removed;
        std::function<void(const PwNode&)> on_stream_target_changed;
        std::function<void()> on_ready;  // first sync after connect completes
        std::function<void()> on_metadata_bound;  // "default" metadata is writable
    };

    PwClient();
    ~PwClient();

    PwClient(const PwClient&) = delete;
    PwClient& operator=(const PwClient&) = delete;

    void set_callbacks(Callbacks cb) { cb_ = std::move(cb); }

    // Connects + starts the thread loop. Throws on failure.
    void start();

    // Stops the thread loop (joins thread).
    void stop();

    // RAII lock around the pw thread loop. Use whenever a foreign thread
    // (e.g. DBus) wants to touch PwClient state or call mutators.
    class Lock {
    public:
        explicit Lock(pw_thread_loop* l) noexcept;
        ~Lock() noexcept;
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
    private:
        pw_thread_loop* loop_;
    };
    Lock lock() noexcept;

    // ---- read-only accessors (call under lock from foreign thread) ----
    const std::map<uint32_t, PwNode>& sinks() const { return sinks_; }
    const std::map<uint32_t, PwNode>& streams() const { return streams_; }
    const PwNode* find_sink_by_node_name(std::string_view node_name) const;

    // ---- mutators (call under lock from foreign thread) ----

    // Set the system default sink to the given node.name. Sets both
    // default.audio.sink and default.configured.audio.sink.
    bool set_default_sink(const std::string& node_name);

    // Move a single stream node to a target sink (by sink object.serial).
    bool move_stream(uint32_t stream_id, uint32_t target_serial);

    // Load module-combine-stream with the given args. Returns module tag
    // for later unload, or empty string on failure.
    bool load_module_combine_stream(const std::string& tag, const std::string& args);

    // Unload a previously loaded module by tag.
    void unload_module(const std::string& tag);

    // Internal — public so C trampolines can reach them.
    void on_global_(uint32_t id, const char* type, uint32_t version, const struct spa_dict* props);
    void on_global_remove_(uint32_t id);
    void on_metadata_property_(uint32_t subject, const char* key, const char* type, const char* value);

private:
    pw_thread_loop* loop_{nullptr};
    pw_context* ctx_{nullptr};
    pw_core* core_{nullptr};
    pw_registry* registry_{nullptr};
    pw_metadata* default_meta_{nullptr};
    pw_proxy* default_meta_proxy_{nullptr};

    // Hooks (heap-allocated so they can be removed cleanly).
    void* registry_listener_{nullptr};
    void* core_listener_{nullptr};
    void* meta_listener_{nullptr};

    int sync_seq_{0};
    bool ready_emitted_{false};

    Callbacks cb_;

    std::map<uint32_t, PwNode> sinks_;     // by id
    std::map<uint32_t, PwNode> streams_;   // by id
    std::vector<PwModule> modules_;

    void bind_node_(uint32_t id, const struct spa_dict* props, bool is_sink);
    void bind_metadata_(uint32_t id, const struct spa_dict* props);
    void issue_sync_();
};

}  // namespace phc
