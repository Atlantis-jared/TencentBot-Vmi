#include "CheckpointStore.h"

#include <chrono>
#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {
    std::uint64_t now_unix_ms() {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(
            duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    }

    void set_err(std::string* err, std::string msg) {
        if (err) *err = std::move(msg);
    }
} // namespace

static void to_json(json& j, const TradingCheckpoint& st) {
    j = json{
        {"version", st.version},
        {"route", st.route},
        {"next_op", st.next_op},
        {"last_op_name", st.last_op_name},
        {"updated_at_ms", st.updated_at_ms},
        {"cycle", st.cycle},
        {"target_money", st.target_money},
        {"preferred_difu_merchant", st.preferred_difu_merchant},
        {"preferred_beiju_merchant", st.preferred_beiju_merchant},
        {"is_goal_reached", st.is_goal_reached},
    };
}

static void from_json(const json& j, TradingCheckpoint& st) {
    st.version = j.value("version", 1);
    st.route = j.value("route", std::string("TradingRoute"));
    st.next_op = j.value("next_op", 0);
    st.last_op_name = j.value("last_op_name", std::string());
    st.updated_at_ms = j.value("updated_at_ms", 0ULL);
    st.cycle = j.value("cycle", 0);
    st.target_money = j.value("target_money", 150000);
    st.preferred_difu_merchant = j.value("preferred_difu_merchant", std::string());
    st.preferred_beiju_merchant = j.value("preferred_beiju_merchant", std::string());
    st.is_goal_reached = j.value("is_goal_reached", false);
}

CheckpointStore::CheckpointStore(std::filesystem::path path) : path_(std::move(path)) {}

bool CheckpointStore::load(TradingCheckpoint& out, std::string* err) const {
    try {
        if (!std::filesystem::exists(path_)) return false;
        std::ifstream in(path_, std::ios::in | std::ios::binary);
        if (!in) {
            set_err(err, "failed to open checkpoint for reading");
            return false;
        }
        json j;
        in >> j;
        out = j.get<TradingCheckpoint>();
        return true;
    } catch (const std::exception& e) {
        set_err(err, std::string("checkpoint load error: ") + e.what());
        return false;
    }
}

bool CheckpointStore::save(const TradingCheckpoint& st, std::string* err) const {
    try {
        TradingCheckpoint copy = st;
        copy.updated_at_ms = now_unix_ms();

        const auto dir = path_.has_parent_path() ? path_.parent_path() : std::filesystem::path{};
        if (!dir.empty()) std::filesystem::create_directories(dir);

        const auto tmp = path_.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!out) {
                set_err(err, "failed to open checkpoint tmp for writing");
                return false;
            }
            json j = copy;
            out << j.dump(2);
            out.flush();
            if (!out) {
                set_err(err, "failed to write checkpoint tmp");
                return false;
            }
        }

        // Replace atomically (best-effort on Windows).
        std::error_code ec;
        std::filesystem::rename(tmp, path_, ec);
        if (ec) {
            // If target exists, remove then retry.
            std::filesystem::remove(path_, ec);
            ec.clear();
            std::filesystem::rename(tmp, path_, ec);
            if (ec) {
                set_err(err, std::string("failed to rename checkpoint tmp: ") + ec.message());
                // Cleanup tmp
                std::filesystem::remove(tmp, ec);
                return false;
            }
        }
        return true;
    } catch (const std::exception& e) {
        set_err(err, std::string("checkpoint save error: ") + e.what());
        return false;
    }
}

bool CheckpointStore::clear(std::string* err) const {
    try {
        if (!std::filesystem::exists(path_)) return true;
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        if (ec) {
            set_err(err, std::string("failed to remove checkpoint: ") + ec.message());
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        set_err(err, std::string("checkpoint clear error: ") + e.what());
        return false;
    }
}

