#pragma once
// Simple JSON checkpoint store for resumable runs.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

struct TradingCheckpoint {
    int version = 3;
    std::string route = "TradingRoute";
    int next_op = 0;                 // next operation index to run
    std::string last_op_name;        // last completed op name (for human readability)
    std::uint64_t updated_at_ms = 0; // unix epoch ms

    int cycle = 0;                   // how many completed trade cycles (difu<->beiju)
    int target_money = 150000;       // stop when money >= target_money
    
    std::string preferred_difu_merchant;  // "地府货商" or "地府商人"
    std::string preferred_beiju_merchant; // "北俱货商" or "北俱商人"

    bool is_goal_reached = false;         // if true, skip trading and go to final return
};

class CheckpointStore {
public:
    explicit CheckpointStore(std::filesystem::path path);

    [[nodiscard]] bool load(TradingCheckpoint& out, std::string* err = nullptr) const;
    [[nodiscard]] bool save(const TradingCheckpoint& st, std::string* err = nullptr) const;
    [[nodiscard]] bool clear(std::string* err = nullptr) const;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

