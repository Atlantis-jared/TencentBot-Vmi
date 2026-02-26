#include "TradingRouteBehavior.h"

#include "../bt/BehaviorTree.h"
#include "../bt/StepDispatchTree.h"

#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
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

// 将一次可能阻塞的业务动作包装为“非阻塞 tick”语义：
// - 第一次 tick：异步启动任务并立即返回 Running
// - 后续 tick：轮询 future，未完成继续 Running
// - 完成后：返回任务最终状态，并清理为下一次可重入
std::function<bt::Status()> make_nonblocking_action(std::function<TreeStatus()> fn) {
    struct AsyncState {
        bool active = false;
        std::future<TreeStatus> future;
    };
    auto state = std::make_shared<AsyncState>();

    return [fn = std::move(fn), state]() mutable -> bt::Status {
        if (!fn) {
            return bt::Status::Failure;
        }
        if (!state->active) {
            state->future = std::async(std::launch::async, [fn]() mutable {
                return fn();
            });
            state->active = true;
            return bt::Status::Running;
        }

        const auto ready = state->future.wait_for(std::chrono::milliseconds(0));
        if (ready != std::future_status::ready) {
            return bt::Status::Running;
        }

        // get() 会把后台任务异常重新抛到 tick 线程，交由上层统一处理。
        const TreeStatus final_status = state->future.get();
        state->active = false;
        return to_bt_status(final_status);
    };
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

        std::vector<std::unique_ptr<bt::Node>> goal_children;
        goal_children.emplace_back(std::make_unique<bt::Condition>(
            "goal_reached?",
            std::move(ctx.is_goal_reached),
            bt_observer
        ));
        goal_children.emplace_back(std::make_unique<bt::Action>(
            "return_and_submit",
            make_nonblocking_action(std::move(ctx.run_goal_action)),
            bt_observer
        ));

        std::vector<bt::StepDispatchItem> dispatch_items;
        dispatch_items.reserve(ctx.steps.size());
        for (auto& step : ctx.steps) {
            dispatch_items.push_back(bt::StepDispatchItem{
                std::move(step.name),
                make_nonblocking_action(std::move(step.run))
            });
        }

        std::unique_ptr<bt::Node> step_dispatch = bt::BuildStepDispatchTree(
            "step_dispatch_branch",
            std::move(dispatch_items),
            std::move(ctx.current_step_index),
            [cycle_restart = std::move(ctx.cycle_restart_if_needed)]() {
                if (!cycle_restart) {
                    return bt::Status::Failure;
                }
                return to_bt_status(cycle_restart());
            },
            bt_observer
        );

        std::vector<std::unique_ptr<bt::Node>> root_children;
        root_children.emplace_back(std::make_unique<bt::Sequence>(
            "goal_branch",
            std::move(goal_children),
            bt_observer
        ));
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
    }
}

} // namespace domain
