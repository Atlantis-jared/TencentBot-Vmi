#pragma once

#include "BehaviorTree.h"

#include <chrono>
#include <memory>
#include <string>

namespace bt {

// ---------------------------------------------------------------------------
// RetryDecorator: 失败自动重试装饰器
// 语义：
// - 子节点 Success => 整体 Success（重置重试计数）
// - 子节点 Running => 整体 Running（透传）
// - 子节点 Failure =>
//     - 未达最大重试次数 => 等待 delay 后重试（返回 Running）
//     - 已达最大重试次数 => 整体 Failure（重置状态）
//
// 等待机制：首次 Failure 记录开始时间，后续 tick 检查是否超过 delay，
// 超过后重新 tick 子节点。
// ---------------------------------------------------------------------------
class RetryDecorator final : public Node {
public:
    // max_retries: 最大重试次数（0=不重试，-1=无限重试）
    // delay: 每次重试前的等待时间
    RetryDecorator(std::string name,
                   std::unique_ptr<Node> child,
                   int max_retries,
                   std::chrono::milliseconds delay,
                   Observer observer = {});

    Status tick() override;

    // 外部重置（用于跨 cycle 清理状态）。
    void reset();

private:
    std::unique_ptr<Node> child_;
    int max_retries_;
    std::chrono::milliseconds delay_;

    int attempts_ = 0;
    bool waiting_ = false;
    std::chrono::steady_clock::time_point wait_start_{};
};

} // namespace bt
