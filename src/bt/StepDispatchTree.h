#pragma once

#include "BehaviorTree.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bt {

// 单个步骤节点定义：
// - step_id: 节点可读名称（用于日志）
// - run    : 当前步骤的执行函数
struct StepDispatchItem {
    std::string step_id;
    std::function<Status()> run;
};

// 构建“按 next_op 分发步骤”的行为树分支：
// 生成结构：
//   Selector(tree_name)
//     - Sequence(is_step_0, run_step_0)
//     - Sequence(is_step_1, run_step_1)
//     - ...
//     - Action(cycle_restart_if_needed)
//
// 约束：
// - current_index_fn 返回当前 checkpoint 的 next_op
// - cycle_restart_fn 仅在 next_op == items.size() 时应返回 Success
std::unique_ptr<Node> BuildStepDispatchTree(
    std::string tree_name,
    std::vector<StepDispatchItem> items,
    std::function<int()> current_index_fn,
    std::function<Status()> cycle_restart_fn,
    Observer observer = {}
);

} // namespace bt

