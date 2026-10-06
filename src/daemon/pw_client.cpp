#include "daemon/pw_client.h"

#include "common/log.h"

#include <pipewire/pipewire.h>
#include <pipewire/impl-module.h>
#include <pipewire/extensions/metadata.h>
#include <spa/utils/dict.h>
#include <spa/utils/hook.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace phc {

namespace {

const char* dict_lookup(const spa_dict* d, const char* key) {
    if (!d) return nullptr;
    const char* v = spa_dict_lookup(d, key);
    return v;
}

uint32_t parse_u32(const char* s, uint32_t fallback = 0) {
    if (!s) return fallback;
    try { return static_cast<uint32_t>(std::stoul(s)); } catch (...) { return fallback; }
}

}  // namespace

// ---------- Lock ----------

PwClient::Lock::Lock(pw_thread_loop* l) noexcept : loop_(l) {
    if (loop_) pw_thread_loop_lock(loop_);
}
PwClient::Lock::~Lock() noexcept {
    if (loop_) pw_thread_loop_unlock(loop_);
}
PwClient::Lock PwClient::lock() noexcept { return Lock{loop_}; }

// ---------- Trampolines ----------

extern "C" {

static void registry_event_global(void* data, uint32_t id, uint32_t /*permissions*/,
                                  const char* type, uint32_t version,
                                  const struct spa_dict* props) {
    static_cast<PwClient*>(data)->on_global_(id, type, version, props);
}

static void registry_event_global_remove(void* data, uint32_t id) {
    static_cast<PwClient*>(data)->on_global_remove_(id);
}

static const struct pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = registry_event_global,
    .global_remove = registry_event_global_remove,
};

static int metadata_property_cb(void* data, uint32_t subject, const char* key,
                                const char* type, const char* value) {
    static_cast<PwClient*>(data)->on_metadata_property_(subject, key, type, value);
    return 0;
}

static const struct pw_metadata_events kMetadataEvents = {
    .version = PW_VERSION_METADATA_EVENTS,
    .property = metadata_property_cb,
};

}  // extern "C"

// ---------- core sync ----------

namespace {
struct CoreCtx { PwClient* self; int* sync_seq; };

extern "C" void core_done_cb(void* data, uint32_t id, int seq) {
    auto* ctx = static_cast<CoreCtx*>(data);
    if (id == PW_ID_CORE && seq == *ctx->sync_seq) {
        ctx->self->on_global_remove_(0);  // no-op; ensures method linkage
    }
}
}  // namespace

// We track readiness manually via a sync round-trip after connect.
// To keep things simple we just use the sync-based ready in on_global_ below.
// (Lightweight: we issue sync after connect; first done event flips ready.)

void PwClient::issue_sync_() {
    sync_seq_ = pw_core_sync(core_, PW_ID_CORE, 0);
}

// ---------- main ----------

PwClient::PwClient() {
    pw_init(nullptr, nullptr);
    loop_ = pw_thread_loop_new("phc-pw", nullptr);
    if (!loop_) throw std::runtime_error("pw_thread_loop_new failed");
}

PwClient::~PwClient() {
    stop();
    if (loop_) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }
    pw_deinit();
}

void PwClient::start() {
    pw_thread_loop_lock(loop_);

    ctx_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!ctx_) {
        pw_thread_loop_unlock(loop_);
        throw std::runtime_error("pw_context_new failed");
    }

    core_ = pw_context_connect(ctx_, nullptr, 0);
    if (!core_) {
        pw_context_destroy(ctx_);
        ctx_ = nullptr;
        pw_thread_loop_unlock(loop_);
        throw std::runtime_error("pw_context_connect failed (is PipeWire running?)");
    }

    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    if (!registry_) {
        pw_core_disconnect(core_); core_ = nullptr;
        pw_context_destroy(ctx_); ctx_ = nullptr;
        pw_thread_loop_unlock(loop_);
        throw std::runtime_error("pw_core_get_registry failed");
    }

    auto* hook = new spa_hook{};
    spa_zero(*hook);
    pw_registry_add_listener(registry_, hook, &kRegistryEvents, this);
    registry_listener_ = hook;

    pw_thread_loop_unlock(loop_);

    if (pw_thread_loop_start(loop_) < 0)
        throw std::runtime_error("pw_thread_loop_start failed");

    // Issue an initial sync so we know when the registry has been enumerated.
    {
        Lock lk(loop_);
        issue_sync_();
    }
}

void PwClient::stop() {
    if (!loop_) return;
    pw_thread_loop_stop(loop_);

    // Tear down everything created on the loop.
    if (registry_listener_) {
        spa_hook_remove(static_cast<spa_hook*>(registry_listener_));
        delete static_cast<spa_hook*>(registry_listener_);
        registry_listener_ = nullptr;
    }
    if (meta_listener_) {
        spa_hook_remove(static_cast<spa_hook*>(meta_listener_));
        delete static_cast<spa_hook*>(meta_listener_);
        meta_listener_ = nullptr;
    }
    if (default_meta_proxy_) {
        pw_proxy_destroy(default_meta_proxy_);
        default_meta_proxy_ = nullptr;
        default_meta_ = nullptr;
    }
    for (auto& m : modules_) {
        if (m.mod) pw_impl_module_destroy(m.mod);
    }
    modules_.clear();
    if (registry_) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
        registry_ = nullptr;
    }
    if (core_) {
        pw_core_disconnect(core_);
        core_ = nullptr;
    }
    if (ctx_) {
        pw_context_destroy(ctx_);
        ctx_ = nullptr;
    }
}

// ---------- registry callbacks ----------

void PwClient::on_global_(uint32_t id, const char* type, uint32_t /*version*/,
                          const spa_dict* props) {
    if (!type) return;

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char* media_class = dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (!media_class) return;
        if (std::strcmp(media_class, "Audio/Sink") == 0) {
            bind_node_(id, props, /*is_sink=*/true);
        } else if (std::strcmp(media_class, "Stream/Output/Audio") == 0) {
            bind_node_(id, props, /*is_sink=*/false);
        }
        return;
    }

    if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char* meta_name = dict_lookup(props, "metadata.name");
        if (meta_name && std::strcmp(meta_name, "default") == 0) {
            bind_metadata_(id, props);
        }
        return;
    }
}

void PwClient::on_global_remove_(uint32_t id) {
    if (auto it = sinks_.find(id); it != sinks_.end()) {
        sinks_.erase(it);
        if (cb_.on_sink_removed) cb_.on_sink_removed(id);
        return;
    }
    if (auto it = streams_.find(id); it != streams_.end()) {
        streams_.erase(it);
        if (cb_.on_stream_removed) cb_.on_stream_removed(id);
        return;
    }
}

// ---------- bind helpers ----------

void PwClient::bind_node_(uint32_t id, const spa_dict* props, bool is_sink) {
    PwNode n;
    n.id = id;
    auto str_or = [&](const char* k) -> std::string {
        const char* v = dict_lookup(props, k);
        return v ? v : "";
    };
    auto bool_of = [&](const char* k) -> bool {
        const char* v = dict_lookup(props, k);
        if (!v) return false;
        return std::strcmp(v, "true") == 0 || std::strcmp(v, "1") == 0;
    };
    n.serial      = parse_u32(dict_lookup(props, PW_KEY_OBJECT_SERIAL));
    n.node_name   = str_or(PW_KEY_NODE_NAME);
    n.description = str_or(PW_KEY_NODE_DESCRIPTION);
    n.media_class = str_or(PW_KEY_MEDIA_CLASS);
    n.app_name    = str_or(PW_KEY_APP_NAME);
    n.is_virtual  = bool_of("node.virtual");

    if (is_sink) {
        log::debug("sink+ id=", id, " serial=", n.serial, " name=", n.node_name);
        auto& slot = sinks_[id] = std::move(n);
        if (cb_.on_sink_added) cb_.on_sink_added(slot);
    } else {
        log::debug("stream+ id=", id, " serial=", n.serial, " app=", n.app_name);
        auto& slot = streams_[id] = std::move(n);
        if (cb_.on_stream_added) cb_.on_stream_added(slot);
    }
}

void PwClient::bind_metadata_(uint32_t id, const spa_dict* /*props*/) {
    if (default_meta_) return;  // already have it

    auto* proxy = static_cast<pw_proxy*>(pw_registry_bind(
        registry_, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
    if (!proxy) {
        log::warn("failed to bind default metadata id=", id);
        return;
    }
    default_meta_proxy_ = proxy;
    default_meta_ = reinterpret_cast<pw_metadata*>(proxy);

    auto* hook = new spa_hook{};
    spa_zero(*hook);
    pw_metadata_add_listener(default_meta_, hook, &kMetadataEvents, this);
    meta_listener_ = hook;

    log::debug("bound default metadata id=", id);
    if (cb_.on_metadata_bound) cb_.on_metadata_bound();
}

// ---------- metadata ----------

void PwClient::on_metadata_property_(uint32_t subject, const char* key,
                                     const char* /*type*/, const char* value) {
    if (!key) return;
    if (std::strcmp(key, "target.object") != 0) return;
    auto it = streams_.find(subject);
    if (it == streams_.end()) return;
    std::string newv = value ? value : "";
    if (it->second.target_object == newv) return;
    it->second.target_object = newv;
    if (cb_.on_stream_target_changed) cb_.on_stream_target_changed(it->second);
}

// ---------- accessors ----------

const PwNode* PwClient::find_sink_by_node_name(std::string_view nn) const {
    // Exact match wins.
    for (auto& [id, n] : sinks_)
        if (n.node_name == nn) return &n;
    // Fall back to PipeWire's disambiguator suffix: live node.name of the
    // form "<nn>.<digits>". ALSA cards re-enumerate with a trailing ".N"
    // (card index / profile collision counter) that isn't stable across
    // boots, so configs typically store the unsuffixed name.
    for (auto& [id, n] : sinks_) {
        if (n.node_name.size() <= nn.size() + 1) continue;
        if (n.node_name.compare(0, nn.size(), nn.data(), nn.size()) != 0) continue;
        if (n.node_name[nn.size()] != '.') continue;
        bool all_digits = true;
        for (size_t i = nn.size() + 1; i < n.node_name.size(); ++i) {
            char c = n.node_name[i];
            if (c < '0' || c > '9') { all_digits = false; break; }
        }
        if (all_digits) return &n;
    }
    return nullptr;
}

// ---------- mutators ----------

bool PwClient::set_default_sink(const std::string& node_name) {
    if (!default_meta_) {
        log::warn("set_default_sink: default metadata not yet available");
        return false;
    }
    std::string json_value = "{\"name\":\"" + node_name + "\"}";
    pw_metadata_set_property(default_meta_, PW_ID_CORE,
        "default.configured.audio.sink", "Spa:String:JSON", json_value.c_str());
    pw_metadata_set_property(default_meta_, PW_ID_CORE,
        "default.audio.sink", "Spa:String:JSON", json_value.c_str());
    log::info("default sink -> ", node_name);
    return true;
}

bool PwClient::move_stream(uint32_t stream_id, uint32_t target_serial) {
    if (!default_meta_) return false;
    std::string val = std::to_string(target_serial);
    pw_metadata_set_property(default_meta_, stream_id, "target.object",
                             "Spa:Id", val.c_str());
    log::debug("move stream id=", stream_id, " -> serial=", target_serial);
    return true;
}

bool PwClient::load_module_combine_stream(const std::string& tag, const std::string& args) {
    if (!ctx_) return false;
    pw_impl_module* mod = pw_context_load_module(
        ctx_, "libpipewire-module-combine-stream", args.c_str(), nullptr);
    if (!mod) {
        log::error("failed to load module-combine-stream tag=", tag);
        return false;
    }
    modules_.push_back(PwModule{tag, mod});
    log::info("loaded combine-stream tag=", tag);
    return true;
}

void PwClient::unload_module(const std::string& tag) {
    for (auto it = modules_.begin(); it != modules_.end(); ++it) {
        if (it->tag == tag) {
            if (it->mod) pw_impl_module_destroy(it->mod);
            modules_.erase(it);
            log::info("unloaded module tag=", tag);
            return;
        }
    }
}

}  // namespace phc
