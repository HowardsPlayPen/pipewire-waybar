#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace phc {

class StateStore {
public:
    explicit StateStore(std::filesystem::path path);

    // Read on construction; missing file is fine.
    std::optional<std::string> chosen() const { return chosen_; }

    // Persist atomically (temp + rename). No-op on identical value.
    void set_chosen(std::optional<std::string> value);

    static std::filesystem::path default_path();

private:
    std::filesystem::path path_;
    std::optional<std::string> chosen_;

    void load();
    void save();
};

}  // namespace phc
