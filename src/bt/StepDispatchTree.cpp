#include "StepDispatchTree.h"

#include <cstddef>
#include <utility>

namespace bt {

std::unique_ptr<Node> BuildStepDispatchTree(
    std::string tree_name,
    std::vector<StepDispatchItem> items,
    std::function<int()> current_index_fn,
    std::function<Status()> cycle_restart_fn,
    Observer observer
) {
    std::vector<std::unique_ptr<Node>> dispatch_children;
    dispatch_children.reserve(items.size() + 1);

    for (std::size_t i = 0; i < items.size(); ++i) {
        const int expected_index = static_cast<int>(i);
        const std::string step_id = items[i].step_id.empty()
                                        ? ("step_" + std::to_string(i))
                                        : items[i].step_id;
        std::function<Status()> run_fn = std::move(items[i].run);

        std::vector<std::unique_ptr<Node>> gated_step_children;
        gated_step_children.reserve(2);
        gated_step_children.emplace_back(std::make_unique<Condition>(
            "is_" + step_id,
            [current_index_fn, expected_index]() {
                return current_index_fn && current_index_fn() == expected_index;
            },
            observer
        ));
        gated_step_children.emplace_back(std::make_unique<Action>(
            "run_" + step_id,
            [run_fn = std::move(run_fn)]() mutable {
                if (!run_fn) {
                    return Status::Failure;
                }
                return run_fn();
            },
            observer
        ));

        dispatch_children.emplace_back(std::make_unique<Sequence>(
            "dispatch_" + step_id,
            std::move(gated_step_children),
            observer
        ));
    }

    const int step_count = static_cast<int>(items.size());
    dispatch_children.emplace_back(std::make_unique<Action>(
        "cycle_restart_if_needed",
        [current_index_fn, cycle_restart_fn, step_count]() {
            if (!current_index_fn || !cycle_restart_fn) {
                return Status::Failure;
            }
            if (current_index_fn() != step_count) {
                return Status::Failure;
            }
            return cycle_restart_fn();
        },
        observer
    ));

    return std::make_unique<Selector>(std::move(tree_name), std::move(dispatch_children), observer);
}

} // namespace bt

