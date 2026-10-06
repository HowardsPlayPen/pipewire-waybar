#pragma once

#include "common/config.h"
#include "common/types.h"
#include "daemon/pw_client.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace phc {

class CombineManager;

// Routes audio according to the user's chosen sink.
//
// Threading: all methods must be called with the PwClient loop locked
// (i.e. wrap with `auto lk = pw_client.lock();`). Internally, callbacks
// from PwClient are already running on the pw thread (lock held).
class Router {
public:
    struct Notify {
        std::function<void(const std::string& chosen)> chosen_changed;
        std::function<void()> sinks_changed;
        std::function<void(const std::string& name, const std::string& type)> device_appeared;
    };

    Router(PwClient& pw, const Config& cfg, CombineManager& cm);

    void set_notify(Notify n) { notify_ = std::move(n); }

    // Apply a new chosen sink (by friendly name). Empty string clears the choice.
    // Returns false if the friendly name is unknown.
    bool choose(const std::string& friendly_name);

    // Returns "" if nothing is chosen.
    const std::string& chosen() const { return chosen_; }

    // Snapshot for DBus ListSinks. Each entry: name, type, node_name, available, isChosen.
    std::vector<SinkSnapshot> snapshot() const;

    // Re-apply enforcement (e.g. after a sink (re)appears or config reloads).
    void reapply();

    // Replace config (e.g. on Reload). Re-resolves chosen if its sink changed/removed.
    void on_config_replaced(const Config& cfg);

    // PwClient event hooks. Called on pw thread.
    void on_sink_added(const PwNode& n);
    void on_sink_removed(uint32_t id);
    void on_stream_added(const PwNode& n);
    void on_stream_target_changed(const PwNode& n);

    // For initial state load (e.g. from StateStore on startup).
    void set_initial_chosen(std::string name) { chosen_ = std::move(name); }

private:
    PwClient& pw_;
    const Config* cfg_;            // non-owning
    CombineManager& cm_;
    Notify notify_;
    std::string chosen_;

    // The node.name currently being targeted (resolved from chosen_; for a
    // virtual sink this is the combine sink's node.name once present).
    std::optional<std::string> target_node_name_for_chosen_() const;

    // Find the live sink node currently representing the chosen friendly name.
    const PwNode* live_chosen_sink_() const;

    // If `nn` is the node.name of a combine-stream internal output we own,
    // returns the SinkSpec of its intended member. nullptr otherwise.
    // Authoritative — the `node.virtual` prop isn't always in registry globals.
    const SinkSpec* combine_member_for_(const std::string& nn) const;

    void enforce_routing_();
    void move_stream_to_chosen_(const PwNode& stream);
    bool sink_is_configured_(const std::string& node_name) const;
};

}  // namespace phc
