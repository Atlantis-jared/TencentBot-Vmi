#pragma once

#include <atomic>
#include <chrono>
#include <exception>
#include <thread>

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

// 协作式等待：
// - 不直接调用系统 Sleep 做长时间阻塞；
// - 以短时间片轮询 stop 信号，确保可以快速中断。
inline void sleep_interruptible(
    const std::atomic_bool* flag,
    int total_ms,
    int slice_ms = 10
) {
    if (total_ms <= 0) {
        check_stop_or_throw(flag);
        return;
    }
    if (slice_ms <= 0) {
        slice_ms = 1;
    }
    int remaining = total_ms;
    while (remaining > 0) {
        check_stop_or_throw(flag);
        const int chunk = (remaining < slice_ms) ? remaining : slice_ms;
        std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
        remaining -= chunk;
    }
}

} // namespace domain
