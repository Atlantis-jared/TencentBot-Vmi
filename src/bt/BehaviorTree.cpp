#include "BehaviorTree.h"

#include <utility>

namespace bt {

// 将状态枚举映射为稳定字符串，便于日志/外部系统解析。
const char* status_to_cstr(Status status) {
    switch (status) {
        case Status::Success: return "Success";
        case Status::Failure: return "Failure";
        case Status::Running: return "Running";
        default: return "Unknown";
    }
}

// 基类构造：保存节点名和观察回调。
Node::Node(std::string name, Observer observer)
    : name_(std::move(name)), observer_(std::move(observer)) {}

// 统一状态上报入口：
// 所有派生节点在返回前都应走 report()，确保观测一致性。
Status Node::report(Status status) const {
    if (observer_) {
        observer_(name_, status);
    }
    return status;
}

// 动作节点：执行动作函数并上报状态。
Action::Action(std::string name, std::function<Status()> fn, Observer observer)
    : Node(std::move(name), std::move(observer)), fn_(std::move(fn)) {}

Status Action::tick() {
    return report(fn_());
}

// 条件节点：true->Success，false->Failure。
Condition::Condition(std::string name, std::function<bool()> fn, Observer observer)
    : Node(std::move(name), std::move(observer)), fn_(std::move(fn)) {}

Status Condition::tick() {
    return report(fn_() ? Status::Success : Status::Failure);
}

// 顺序节点：
// 1) 保持 index_ 支持跨 tick 的 Running 恢复
// 2) 失败或成功后重置 index_，保证下次从头执行
Sequence::Sequence(std::string name, std::vector<std::unique_ptr<Node>> children, Observer observer)
    : Node(std::move(name), std::move(observer)), children_(std::move(children)) {}

Status Sequence::tick() {
    while (index_ < children_.size()) {
        const Status s = children_[index_]->tick();
        if (s == Status::Running) return report(Status::Running);
        if (s == Status::Failure) {
            index_ = 0;
            return report(Status::Failure);
        }
        ++index_;
    }
    index_ = 0;
    return report(Status::Success);
}

// 选择节点：
// 1) 优先执行前面的子节点（左侧优先）
// 2) Success 即短路返回
// 3) Running 保持当前位置等待下次继续
Selector::Selector(std::string name, std::vector<std::unique_ptr<Node>> children, Observer observer)
    : Node(std::move(name), std::move(observer)), children_(std::move(children)) {}

Status Selector::tick() {
    while (index_ < children_.size()) {
        const Status s = children_[index_]->tick();
        if (s == Status::Running) return report(Status::Running);
        if (s == Status::Success) {
            index_ = 0;
            return report(Status::Success);
        }
        ++index_;
    }
    index_ = 0;
    return report(Status::Failure);
}

} // namespace bt
