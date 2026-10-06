#include "daemon/combine_manager.h"

#include "common/log.h"

#include <sstream>
#include <string_view>

namespace phc {

namespace {

// Build a regex (for PipeWire's "~regex" match form) that matches a configured
// node.name and tolerates ALSA's trailing ".<digits>" disambiguator. The
// returned string is intended to go inside a SPA-JSON double-quoted value, so
// every backslash is doubled — SPA-JSON parsing collapses "\\" to "\" before
// the regex engine sees it.
std::string node_name_regex_for_spa_json(std::string_view configured) {
    std::string out = "~^";
    for (char c : configured) {
        switch (c) {
            case '.': case '\\': case '+': case '*': case '?':
            case '(': case ')': case '[': case ']': case '{': case '}':
            case '|': case '^': case '$': case '/':
                out += "\\\\";  // SPA-JSON \\ -> \, regex escapes the meta-char
                out += c;
                break;
            default:
                out += c;
                break;
        }
    }
    out += "(\\\\.[0-9]+)?$";
    return out;
}

}  // namespace

CombineManager::CombineManager(PwClient& pw) : pw_(pw) {}

std::string CombineManager::slug_of(const std::string& friendly) {
    std::string s;
    s.reserve(friendly.size());
    for (char c : friendly) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) s += c;
        else if (c >= 'A' && c <= 'Z') s += static_cast<char>(c | 0x20);
        else s += '_';
    }
    return s;
}

std::string CombineManager::args_for(const SinkSpec& vs, const Config& cfg) {
    // Build SPA-JSON-ish module args. PipeWire's parser accepts this
    // relaxed bracket-and-key form.
    std::ostringstream a;
    a << "{ ";
    a << "combine.mode = sink ";
    a << "node.name = phc_combine_" << slug_of(vs.name) << " ";
    a << "node.description = \"" << vs.name << "\" ";
    a << "combine.latency-compensate = false ";
    a << "combine.props = { audio.position = [ FL FR ] } ";
    a << "stream.props = { } ";
    a << "stream.rules = [ ";
    for (auto& m : vs.members) {
        const SinkSpec* ms = cfg.find(m);
        if (!ms || ms->kind != SinkKind::Real) continue;
        a << "{ matches = [ { media.class = \"Audio/Sink\" "
          << "node.name = \"" << node_name_regex_for_spa_json(ms->node_name) << "\" } ] "
          << "actions = { create-stream = { "
          // audio.position + combine.audio.position are required for the
          // module's auto-linker (and wireplumber) to wire the internal
          // output ports to the member sink's playback ports.
          << "audio.position = [ FL FR ] "
          << "combine.audio.position = [ FL FR ] "
          << "} } } ";
    }
    a << "]";
    a << " }";
    return a.str();
}

void CombineManager::load_all(const Config& cfg) {
    for (auto& s : cfg.sinks) {
        if (s.kind != SinkKind::Virtual) continue;
        if (loaded_tags_.count(s.name)) continue;
        std::string args = args_for(s, cfg);
        log::debug("combine args[", s.name, "]: ", args);
        if (pw_.load_module_combine_stream(s.name, args))
            loaded_tags_.insert(s.name);
    }
}

void CombineManager::unload_all() {
    for (auto& tag : loaded_tags_) pw_.unload_module(tag);
    loaded_tags_.clear();
}

void CombineManager::reconcile(const Config& old_cfg, const Config& new_cfg) {
    auto virt_set = [](const Config& c) {
        std::set<std::string> out;
        for (auto& s : c.sinks)
            if (s.kind == SinkKind::Virtual) out.insert(s.name);
        return out;
    };
    auto find_virt = [](const Config& c, const std::string& name) -> const SinkSpec* {
        const SinkSpec* p = c.find(name);
        return (p && p->kind == SinkKind::Virtual) ? p : nullptr;
    };

    auto old_v = virt_set(old_cfg);
    auto new_v = virt_set(new_cfg);

    // Unload removed.
    for (auto& tag : old_v) {
        if (!new_v.count(tag) && loaded_tags_.count(tag)) {
            pw_.unload_module(tag);
            loaded_tags_.erase(tag);
        }
    }
    // Replace changed (members, or a member's node_name, differ).
    for (auto& tag : new_v) {
        if (!old_v.count(tag)) continue;
        auto* o = find_virt(old_cfg, tag);
        auto* n = find_virt(new_cfg, tag);
        if (!o || !n) continue;
        if (args_for(*o, old_cfg) != args_for(*n, new_cfg)) {
            if (loaded_tags_.count(tag)) {
                pw_.unload_module(tag);
                loaded_tags_.erase(tag);
            }
        }
    }
    // Load added or replaced.
    for (auto& tag : new_v) {
        if (loaded_tags_.count(tag)) continue;
        auto* n = find_virt(new_cfg, tag);
        if (!n) continue;
        if (pw_.load_module_combine_stream(tag, args_for(*n, new_cfg)))
            loaded_tags_.insert(tag);
    }
}

}  // namespace phc
