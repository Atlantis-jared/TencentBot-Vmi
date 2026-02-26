#include "WaitAction.h"

#include <utility>

namespace bt {

WaitAction::WaitAction(std::string name,
                       std::chrono::milliseconds duration,
                       Observer observer)
    : Node(std::move(name), std::move(observer)),
      duration_(duration) {}

Status WaitAction::tick() {
    if (!started_) {
        started_ = true;
        start_time_ = std::chrono::steady_clock::now();
        return report(Status::Running);
    }

    const auto elapsed = std::chrono::steady_clock::now() - start_time_;
    if (elapsed < duration_) {
        return report(Status::Running);
    }

    // 等待完成，重置供下次使用。
    started_ = false;
    return report(Status::Success);
}

void WaitAction::reset() {
    started_ = false;
}

} // namespace bt
