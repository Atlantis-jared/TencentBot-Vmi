#pragma once

#include "BehaviorTree.h"

#include <chrono>
#include <string>

namespace bt {

// ---------------------------------------------------------------------------
// WaitAction: 定时等待节点
// 语义：
//   首次 tick：记录开始时间，返回 Running
//   后续 tick：检查已过时间，未到 => Running，已到 => Success 并重置
// 用于行为树中需要"等一段时间后再继续"的场景。
// ---------------------------------------------------------------------------
class WaitAction final : public Node {
public:
    WaitAction(std::string name,
               std::chrono::milliseconds duration,
               Observer observer = {});

    Status tick() override;

    // 重置计时器（用于外部重新开始等待）。
    void reset();

private:
    std::chrono::milliseconds duration_;
    bool started_ = false;
    std::chrono::steady_clock::time_point start_time_{};
};

} // namespace bt
