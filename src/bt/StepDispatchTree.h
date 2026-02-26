#pragma once

#include "BehaviorTree.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bt {

// 单个步骤节点定义（函数模式）：
// - step_id: 节点可读名称（用于日志）
// - run    : 当前步骤的执行函数
struct StepDispatchItem {
    std::string step_id;
    std::function<Status()> run;
};

// 单个步骤节点定义（子树模式）：
// - step_id: 节点可读名称（用于日志）
// - subtree: 预构建的行为树子树
struct StepTreeItem {
    std::string step_id;
    std::unique_ptr<Node> subtree;
};

// 构建"按 next_op 分发步骤"的行为树分支（函数模式）：
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

// 构建"按 next_op 分发步骤"的行为树分支（子树模式）：
// 与上方函数模式语义相同，但每个步骤直接使用预构建的行为树子树，
// 而非包装为 Action 节点。
std::unique_ptr<Node> BuildStepDispatchTree(
    std::string tree_name,
    std::vector<StepTreeItem> items,
    std::function<int()> current_index_fn,
    std::function<Status()> cycle_restart_fn,
    Observer observer = {}
);

} // namespace bt
