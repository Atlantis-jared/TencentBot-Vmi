#include "TradingRouteBehavior.h"

#include "../bt/BehaviorTree.h"
#include "../bt/StepDispatchTree.h"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace domain {

namespace {

bt::Status to_bt_status(TreeStatus status) {
    switch (status) {
        case TreeStatus::Success: return bt::Status::Success;
        case TreeStatus::Failure: return bt::Status::Failure;
        case TreeStatus::Running: return bt::Status::Running;
        default: return bt::Status::Failure;
    }
}

TreeStatus from_bt_status(bt::Status status) {
    switch (status) {
        case bt::Status::Success: return TreeStatus::Success;
        case bt::Status::Failure: return TreeStatus::Failure;
        case bt::Status::Running: return TreeStatus::Running;
        default: return TreeStatus::Failure;
    }
}

} // namespace

const char* status_to_cstr(TreeStatus status) {
    switch (status) {
        case TreeStatus::Success: return "Success";
        case TreeStatus::Failure: return "Failure";
        case TreeStatus::Running: return "Running";
        default: return "Unknown";
    }
}

class TradingRouteBehavior::Impl final {
public:
    explicit Impl(TradingTreeBuildContext ctx) {
        bt::Observer bt_observer = [observer = std::move(ctx.observer)](std::string_view node_name, bt::Status status) {
            if (observer) {
                observer(node_name, from_bt_status(status));
            }
        };

        // --- goal 分支 ---
        std::unique_ptr<bt::Node> goal_branch_node;
        if (ctx.goal_subtree) {
            // 子树模式：goal 判断和动作都已在子树内部构建好。
            goal_branch_node = std::move(ctx.goal_subtree);
        } else {
            // 函数模式：用 Condition + Action 构建 goal 分支。
            std::vector<std::unique_ptr<bt::Node>> goal_children;
            goal_children.emplace_back(std::make_unique<bt::Condition>(
                "goal_reached?",
                std::move(ctx.is_goal_reached),
                bt_observer
            ));
            goal_children.emplace_back(std::make_unique<bt::Action>(
                "return_and_submit",
                [goal_action = std::move(ctx.run_goal_action)]() {
                    if (!goal_action) {
                        return bt::Status::Failure;
                    }
                    return to_bt_status(goal_action());
                },
                bt_observer
            ));
            goal_branch_node = std::make_unique<bt::Sequence>(
                "goal_branch",
                std::move(goal_children),
                bt_observer
            );
        }

        // --- 步骤分发分支 ---
        auto cycle_restart_bt = [cycle_restart = std::move(ctx.cycle_restart_if_needed)]() {
            if (!cycle_restart) {
                return bt::Status::Failure;
            }
            return to_bt_status(cycle_restart());
        };

        std::unique_ptr<bt::Node> step_dispatch;
        if (!ctx.step_subtrees.empty()) {
            // 子树模式：将 StepSubtreeNode 转换为 bt::StepTreeItem。
            std::vector<bt::StepTreeItem> tree_items;
            tree_items.reserve(ctx.step_subtrees.size());
            for (auto& sn : ctx.step_subtrees) {
                tree_items.push_back(bt::StepTreeItem{
                    std::move(sn.name),
                    std::move(sn.subtree)
                });
            }
            step_dispatch = bt::BuildStepDispatchTree(
                "step_dispatch_branch",
                std::move(tree_items),
                std::move(ctx.current_step_index),
                std::move(cycle_restart_bt),
                bt_observer
            );
        } else {
            // 函数模式（向后兼容）：将 TradingStepNode 包装为 bt::StepDispatchItem。
            std::vector<bt::StepDispatchItem> dispatch_items;
            dispatch_items.reserve(ctx.steps.size());
            for (auto& step : ctx.steps) {
                dispatch_items.push_back(bt::StepDispatchItem{
                    std::move(step.name),
                    [run = std::move(step.run)]() {
                        if (!run) {
                            return bt::Status::Failure;
                        }
                        return to_bt_status(run());
                    }
                });
            }
            step_dispatch = bt::BuildStepDispatchTree(
                "step_dispatch_branch",
                std::move(dispatch_items),
                std::move(ctx.current_step_index),
                std::move(cycle_restart_bt),
                bt_observer
            );
        }

        // --- 根节点 ---
        std::vector<std::unique_ptr<bt::Node>> root_children;
        root_children.emplace_back(std::move(goal_branch_node));
        root_children.emplace_back(std::move(step_dispatch));
        root_ = std::make_unique<bt::Selector>("route_root", std::move(root_children), bt_observer);
    }

    TreeStatus Tick() {
        if (!root_) {
            throw std::runtime_error("trading route root is null");
        }
        return from_bt_status(root_->tick());
    }

private:
    std::unique_ptr<bt::Node> root_;
};

TradingRouteBehavior::TradingRouteBehavior(TradingTreeBuildContext ctx)
    : impl_(std::make_unique<Impl>(std::move(ctx))) {}

TradingRouteBehavior::~TradingRouteBehavior() = default;
TradingRouteBehavior::TradingRouteBehavior(TradingRouteBehavior&&) noexcept = default;
TradingRouteBehavior& TradingRouteBehavior::operator=(TradingRouteBehavior&&) noexcept = default;

TreeStatus TradingRouteBehavior::Tick() {
    return impl_->Tick();
}

void RunTradingRouteTreeLoop(
    TradingRouteBehavior& behavior,
    const std::function<void()>& check_stop_fn,
    const std::function<void()>& on_failure_tick
) {
    while (true) {
        if (check_stop_fn) {
            check_stop_fn();
        }
        const TreeStatus status = behavior.Tick();
        if (status == TreeStatus::Failure && on_failure_tick) {
            on_failure_tick();
        }
        if (status == TreeStatus::Running) {
            // 协作式非阻塞执行：Running 态短暂让出 CPU，避免空转。
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

} // namespace domain
