#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace domain {

// domain 层对外暴露的行为树状态，不泄漏 bt 命名空间类型。
enum class TreeStatus {
    Success,
    Failure,
    Running
};

const char* status_to_cstr(TreeStatus status);

// 单个业务步骤定义：
// - name: 行为树节点展示名（用于日志）
// - run : 执行该步骤的函数，返回行为树状态
struct TradingStepNode {
    std::string name;
    std::function<TreeStatus()> run;
};

using TreeObserver = std::function<void(std::string_view node_name, TreeStatus status)>;

// 行为树构建参数：
// 将业务层状态和动作通过回调注入，避免 domain 层直接依赖 TencentBot 内部实现。
struct TradingTreeBuildContext {
    std::function<bool()> is_goal_reached;
    std::function<TreeStatus()> run_goal_action;

    std::vector<TradingStepNode> steps;
    std::function<int()> current_step_index;
    std::function<TreeStatus()> cycle_restart_if_needed;

    TreeObserver observer;
};

class TradingRouteBehavior final {
public:
    explicit TradingRouteBehavior(TradingTreeBuildContext ctx);
    ~TradingRouteBehavior();
    TradingRouteBehavior(TradingRouteBehavior&&) noexcept;
    TradingRouteBehavior& operator=(TradingRouteBehavior&&) noexcept;

    TradingRouteBehavior(const TradingRouteBehavior&) = delete;
    TradingRouteBehavior& operator=(const TradingRouteBehavior&) = delete;

    TreeStatus Tick();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// 统一行为树执行循环：
// - 每 tick 前执行 check_stop_fn（通常会在外部抛 stop 异常）
// - tick 失败时执行 on_failure_tick（通常记录日志+Sleep）
void RunTradingRouteTreeLoop(
    TradingRouteBehavior& behavior,
    const std::function<void()>& check_stop_fn,
    const std::function<void()>& on_failure_tick
);

} // namespace domain
