#pragma once

#include "common/config.h"
#include "daemon/pw_client.h"

#include <set>
#include <string>

namespace phc {

// Manages module-combine-stream instances for virtual sinks defined in config.
// Strategy: load all virtuals at startup and leave loaded (idle cost is
// negligible). On config reload, diff and load/unload the changes.
//
// Threading: methods must be called with the PwClient loop locked.
class CombineManager {
public:
    CombineManager(PwClient& pw);

    // Load combine-stream modules for all virtual sinks in cfg.
    void load_all(const Config& cfg);

    // Unload all currently loaded combine modules.
    void unload_all();

    // Diff against new config: unload removed, load added, replace changed.
    void reconcile(const Config& old_cfg, const Config& new_cfg);

private:
    PwClient& pw_;
    std::set<std::string> loaded_tags_;  // friendly names of currently loaded virtuals

    static std::string args_for(const SinkSpec& vs, const Config& cfg);
    static std::string slug_of(const std::string& friendly);
};

}  // namespace phc
