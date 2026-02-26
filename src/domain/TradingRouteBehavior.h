#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bt { class Node; }

namespace domain {

// domain 层对外暴露的行为树状态，不泄漏 bt 命名空间类型。
enum class TreeStatus {
    Success,
    Failure,
    Running
};

const char* status_to_cstr(TreeStatus status);

// 单个业务步骤定义（函数模式）：
// - name: 行为树节点展示名（用于日志）
// - run : 执行该步骤的函数，返回行为树状态
struct TradingStepNode {
    std::string name;
    std::function<TreeStatus()> run;
};

// 单个业务步骤定义（子树模式）：
// - name   : 行为树节点展示名（用于日志）
// - subtree: 预构建的行为树子树节点（所有权转移给 TradingRouteBehavior）
struct StepSubtreeNode {
    std::string name;
    std::unique_ptr<bt::Node> subtree;
};

using TreeObserver = std::function<void(std::string_view node_name, TreeStatus status)>;

// 行为树构建参数：
// 将业务层状态和动作通过回调注入，避免 domain 层直接依赖 TencentBot 内部实现。
// 支持两种模式：
//   1. 函数模式 — steps 非空：每个 step 是一个返回 TreeStatus 的函数
//   2. 子树模式 — step_subtrees 非空：每个 step 是一个预构建的 bt::Node 子树
// 两者互斥，优先使用 step_subtrees。goal_branch 也支持子树模式（goal_subtree）。
struct TradingTreeBuildContext {
    // goal 判断 + 动作（函数模式）
    std::function<bool()> is_goal_reached;
    std::function<TreeStatus()> run_goal_action;
    // goal 子树（子树模式，与上面两个互斥，优先使用）
    std::unique_ptr<bt::Node> goal_subtree;

    // 步骤列表（函数模式）
    std::vector<TradingStepNode> steps;
    // 步骤列表（子树模式，优先使用）
    std::vector<StepSubtreeNode> step_subtrees;

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
