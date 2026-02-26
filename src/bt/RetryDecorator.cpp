#include "RetryDecorator.h"

#include <utility>

namespace bt {

RetryDecorator::RetryDecorator(std::string name,
                               std::unique_ptr<Node> child,
                               int max_retries,
                               std::chrono::milliseconds delay,
                               Observer observer)
    : Node(std::move(name), std::move(observer)),
      child_(std::move(child)),
      max_retries_(max_retries),
      delay_(delay) {}

Status RetryDecorator::tick() {
    // 如果正在等待重试间隔，检查是否已到期。
    if (waiting_) {
        const auto elapsed = std::chrono::steady_clock::now() - wait_start_;
        if (elapsed < delay_) {
            return report(Status::Running);
        }
        waiting_ = false;
        // 等待结束，继续执行子节点。
    }

    const Status s = child_->tick();

    if (s == Status::Success) {
        // 成功：重置所有重试状态。
        attempts_ = 0;
        waiting_ = false;
        return report(Status::Success);
    }

    if (s == Status::Running) {
        return report(Status::Running);
    }

    // Failure: 检查是否可以重试。
    ++attempts_;
    if (max_retries_ >= 0 && attempts_ > max_retries_) {
        // 已用尽重试次数。
        attempts_ = 0;
        waiting_ = false;
        return report(Status::Failure);
    }

    // 进入等待状态，下次 tick 会检查 delay 是否到期。
    waiting_ = true;
    wait_start_ = std::chrono::steady_clock::now();
    return report(Status::Running);
}

void RetryDecorator::reset() {
    attempts_ = 0;
    waiting_ = false;
}

} // namespace bt
