#pragma once

#include <atomic>
#include <exception>

namespace domain {

class StopRequestedException final : public std::exception {
public:
    const char* what() const noexcept override { return "stop requested"; }
};

class GoalReachedException final : public std::exception {
public:
    const char* what() const noexcept override { return "goal reached"; }
};

inline bool is_stop_requested(const std::atomic_bool* flag) {
    return flag && flag->load(std::memory_order_relaxed);
}

[[noreturn]] inline void throw_stop_requested() {
    throw StopRequestedException{};
}

inline void check_stop_or_throw(const std::atomic_bool* flag) {
    if (is_stop_requested(flag)) {
        throw_stop_requested();
    }
}

[[noreturn]] inline void throw_goal_reached() {
    throw GoalReachedException{};
}

} // namespace domain

