#include "daemon/router.h"

#include "common/log.h"
#include "daemon/combine_manager.h"

namespace phc {

namespace {

// True if a live PipeWire node.name matches a configured node.name. Accepts
// PipeWire's trailing ".<digits>" disambiguator — same rule as
// PwClient::find_sink_by_node_name's fallback.
bool node_name_matches(std::string_view configured, std::string_view live) {
    if (configured == live) return true;
    if (live.size() <= configured.size() + 1) return false;
    if (live.compare(0, configured.size(), configured) != 0) return false;
    if (live[configured.size()] != '.') return false;
    for (size_t i = configured.size() + 1; i < live.size(); ++i) {
        char c = live[i];
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// node.name for a virtual sink as loaded by CombineManager.
std::string virtual_node_name(const std::string& friendly) {
    std::string s = "phc_combine_";
    for (char c : friendly) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            s += static_cast<char>(c | 0x20);
        else
            s += '_';
    }
    return s;
}

}  // namespace

Router::Router(PwClient& pw, const Config& cfg, CombineManager& cm)
    : pw_(pw), cfg_(&cfg), cm_(cm) {}

std::optional<std::string> Router::target_node_name_for_chosen_() const {
    if (chosen_.empty()) return std::nullopt;
    const SinkSpec* s = cfg_->find(chosen_);
    if (!s) return std::nullopt;
    if (s->kind == SinkKind::Virtual) return virtual_node_name(s->name);
    return s->node_name;
}

const PwNode* Router::live_chosen_sink_() const {
    auto target = target_node_name_for_chosen_();
    if (!target) return nullptr;
    return pw_.find_sink_by_node_name(*target);
}

const SinkSpec* Router::combine_member_for_(const std::string& nn) const {
    static const std::string kOut = "output.";
    if (nn.compare(0, kOut.size(), kOut) != 0) return nullptr;
    for (auto& vs : cfg_->sinks) {
        if (vs.kind != SinkKind::Virtual) continue;
        std::string sink_nn = virtual_node_name(vs.name);  // phc_combine_<slug>
        std::string prefix  = kOut + sink_nn + "_";
        if (nn.compare(0, prefix.size(), prefix) != 0) continue;
        std::string member_nn = nn.substr(prefix.size());
        for (auto& mname : vs.members) {
            const SinkSpec* ms = cfg_->find(mname);
            if (ms && ms->node_name == member_nn) return ms;
        }
    }
    return nullptr;
}

bool Router::sink_is_configured_(const std::string& node_name) const {
    for (auto& s : cfg_->sinks) {
        if (s.kind == SinkKind::Real && node_name_matches(s.node_name, node_name)) return true;
        if (s.kind == SinkKind::Virtual && node_name_matches(virtual_node_name(s.name), node_name)) return true;
    }
    return false;
}

void Router::enforce_routing_() {
    if (chosen_.empty()) return;
    const PwNode* target = live_chosen_sink_();
    if (!target) {
        log::info("router: chosen='", chosen_,
                  "' has no live sink; not enforcing routing");
        return;
    }
    pw_.set_default_sink(target->node_name);
    for (auto& [id, st] : pw_.streams()) {
        if (st.is_virtual) continue;
        // node.virtual isn't always in registry-global props, so back it up
        // with a name-prefix check on combine-stream internals.
        if (combine_member_for_(st.node_name)) continue;
        // Move every currently-active stream regardless of pinning, since
        // the user explicitly switched. The "respect target.object" rule
        // only applies to *new* streams that arrive later.
        pw_.move_stream(id, target->serial);
    }
}

bool Router::choose(const std::string& friendly_name) {
    if (!friendly_name.empty() && !cfg_->find(friendly_name)) {
        log::warn("router: unknown sink: ", friendly_name);
        return false;
    }
    bool changed = (chosen_ != friendly_name);
    chosen_ = friendly_name;
    enforce_routing_();
    if (changed && notify_.chosen_changed) notify_.chosen_changed(chosen_);
    return true;
}

void Router::reapply() {
    enforce_routing_();
}

std::vector<SinkSnapshot> Router::snapshot() const {
    std::vector<SinkSnapshot> out;
    out.reserve(cfg_->sinks.size());
    for (auto& s : cfg_->sinks) {
        SinkSnapshot snap;
        snap.name = s.name;
        snap.type = s.type;
        if (s.kind == SinkKind::Real) {
            snap.node_name = s.node_name;
            snap.available = (pw_.find_sink_by_node_name(s.node_name) != nullptr);
        } else {
            std::string vnn = virtual_node_name(s.name);
            snap.node_name = vnn;
            // A virtual sink is "available" if its combine sink node is present
            // in the PW graph AND at least one member is live.
            bool present = (pw_.find_sink_by_node_name(vnn) != nullptr);
            bool any_member = false;
            for (auto& m : s.members) {
                const SinkSpec* ms = cfg_->find(m);
                if (ms && pw_.find_sink_by_node_name(ms->node_name)) {
                    any_member = true;
                    break;
                }
            }
            snap.available = present && any_member;
        }
        snap.chosen = (s.name == chosen_);
        out.push_back(std::move(snap));
    }
    return out;
}

void Router::on_config_replaced(const Config& cfg) {
    cfg_ = &cfg;
    if (!chosen_.empty() && !cfg_->find(chosen_)) {
        log::warn("router: chosen '", chosen_, "' not in new config; clearing");
        chosen_.clear();
        if (notify_.chosen_changed) notify_.chosen_changed(chosen_);
    }
    if (notify_.sinks_changed) notify_.sinks_changed();
    enforce_routing_();
}

void Router::on_sink_added(const PwNode& n) {
    if (!sink_is_configured_(n.node_name)) return;

    // Find friendly + type for the appeared device.
    std::string friendly, type;
    for (auto& s : cfg_->sinks) {
        if (s.kind == SinkKind::Real && node_name_matches(s.node_name, n.node_name)) {
            friendly = s.name; type = s.type; break;
        }
        if (s.kind == SinkKind::Virtual && node_name_matches(virtual_node_name(s.name), n.node_name)) {
            friendly = s.name; type = s.type; break;
        }
    }

    if (notify_.sinks_changed) notify_.sinks_changed();
    if (!friendly.empty() && notify_.device_appeared)
        notify_.device_appeared(friendly, type);

    // If the chosen sink just (re)appeared, reapply enforcement.
    auto target = target_node_name_for_chosen_();
    if (target && node_name_matches(*target, n.node_name)) {
        log::info("router: chosen sink reappeared; reapplying");
        enforce_routing_();
    }
}

void Router::on_sink_removed(uint32_t /*id*/) {
    // We don't get the node.name here directly; just signal a refresh.
    // Per design (answer 13): if the chosen sink disappears, stop enforcing
    // and do NOT auto-switch — leave PipeWire's default in place.
    if (notify_.sinks_changed) notify_.sinks_changed();
}

void Router::on_stream_added(const PwNode& n) {
    // Combine-stream internal output? Pin its target.object back to its
    // designated member. This both initialises new streams correctly and
    // repairs stale metadata left by earlier (buggier) daemon runs.
    if (const SinkSpec* member = combine_member_for_(n.node_name)) {
        const PwNode* live = pw_.find_sink_by_node_name(member->node_name);
        if (live) pw_.move_stream(n.id, live->serial);
        return;
    }
    if (n.is_virtual) return;

    // New-stream policy (interpretation b): respect explicit target.object,
    // otherwise route to the chosen sink.
    if (!n.target_object.empty()) return;

    const PwNode* target = live_chosen_sink_();
    if (!target) return;
    pw_.move_stream(n.id, target->serial);
}

void Router::on_stream_target_changed(const PwNode& /*n*/) {
    // No-op for now; we let users re-pin streams freely. The next time a
    // user picks a sink, we'll move everything again.
}

void Router::move_stream_to_chosen_(const PwNode& stream) {
    const PwNode* target = live_chosen_sink_();
    if (!target) return;
    pw_.move_stream(stream.id, target->serial);
}

}  // namespace phc
