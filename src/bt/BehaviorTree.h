#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bt {

// ---------------------------------------------------------------------------
// 行为树节点返回状态：
//   Success: 节点执行成功（本 tick 完成）
//   Failure: 节点执行失败（本 tick 结束）
//   Running: 节点仍在进行中（需要下一次 tick 继续）
// ---------------------------------------------------------------------------
enum class Status {
    Success,
    Failure,
    Running
};

// 将状态枚举转为可读字符串，主要用于日志/监控输出。
const char* status_to_cstr(Status status);

// 节点观察回调：
// 每个节点 tick 后都会将 (节点名, 状态) 回调出去，便于记录行为树轨迹。
using Observer = std::function<void(std::string_view node_name, Status status)>;

// ---------------------------------------------------------------------------
// Node: 行为树抽象基类
// 设计要点：
// 1) 每个节点都有 name_，用于可观测日志
// 2) 每个节点可选 observer_，用于输出状态变化
// 3) 由派生类实现 tick()，Node 只负责 report() 统一上报
// ---------------------------------------------------------------------------
class Node {
public:
    Node(std::string name, Observer observer = {});
    virtual ~Node() = default;
    virtual Status tick() = 0;

protected:
    Status report(Status status) const;

private:
    std::string name_;
    Observer observer_;
};

// ---------------------------------------------------------------------------
// Action: 动作节点
// 执行传入的函数 fn_ 并返回其状态，适用于“做一件具体事情”。
// ---------------------------------------------------------------------------
class Action final : public Node {
public:
    Action(std::string name, std::function<Status()> fn, Observer observer = {});
    Status tick() override;

private:
    std::function<Status()> fn_;
};

// ---------------------------------------------------------------------------
// Condition: 条件节点
// 执行布尔函数 fn_：
//   true  -> Success
//   false -> Failure
// 通常用于门控判断（例如 goal_reached?）。
// ---------------------------------------------------------------------------
class Condition final : public Node {
public:
    Condition(std::string name, std::function<bool()> fn, Observer observer = {});
    Status tick() override;

private:
    std::function<bool()> fn_;
};

// ---------------------------------------------------------------------------
// Sequence: 顺序节点
// 语义（从左到右）：
// - 任一子节点 Failure => 整体 Failure，并重置游标 index_=0
// - 任一子节点 Running => 整体 Running，保留当前 index_（下次继续）
// - 全部 Success        => 整体 Success，并重置游标 index_=0
// ---------------------------------------------------------------------------
class Sequence final : public Node {
public:
    Sequence(std::string name, std::vector<std::unique_ptr<Node>> children, Observer observer = {});
    Status tick() override;

private:
    std::vector<std::unique_ptr<Node>> children_;
    std::size_t index_ = 0;
};

// ---------------------------------------------------------------------------
// Selector: 选择节点（优先级选择）
// 语义（从左到右）：
// - 任一子节点 Success => 整体 Success，并重置游标 index_=0
// - 任一子节点 Running => 整体 Running，保留当前 index_（下次继续）
// - 全部 Failure        => 整体 Failure，并重置游标 index_=0
// ---------------------------------------------------------------------------
class Selector final : public Node {
public:
    Selector(std::string name, std::vector<std::unique_ptr<Node>> children, Observer observer = {});
    Status tick() override;

private:
    std::vector<std::unique_ptr<Node>> children_;
    std::size_t index_ = 0;
};

} // namespace bt
