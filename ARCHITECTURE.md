# TencentBot-vmi Industrial Architecture Baseline

本文件定义 TencentBot-vmi 的“工业级”基线，后续所有重构都以此为准。

## 1. 分层原则

- `Presentation`：远程控制协议（INIT/START/STOP/STATUS/QUIT）
- `Application`：生命周期状态机（NotInitialized/Initializing/Idle/Running/InitFailed）
- `Domain`：跑商策略（行为树）
- `Infrastructure`：内存读取、输入模拟、视觉识别、DXGI 捕获、AI 验证码

禁止跨层直接耦合：

- 控制层不能直接调用底层细节（只通过 `TencentBot::init/runTradingRoute`）
- 策略层不能直接访问网络层
- 业务文件 `TencentBot.cpp` 不直接依赖 `src/bt/*`，统一通过 `src/domain/TradingRouteBehavior.*` 门面访问行为树

## 2. 生命周期状态机

状态：

- `NOT_INITIALIZED`
- `INITIALIZING`
- `IDLE`
- `RUNNING`
- `INIT_FAILED`

命令约束：

- `INIT` 仅在 `NOT_INITIALIZED/INIT_FAILED` 有效
- `START` 仅在 `IDLE` 有效
- `STOP` 仅请求停止，不触发状态跳变到未初始化
- `STATUS` 必须可重入、无副作用

## 3. 跑商策略（行为树）

`runTradingRoute()` 使用行为树驱动：

- `Selector`
  - `Sequence(goal_reached_condition -> return_and_submit_action)`
  - `Selector(step_dispatch_branch)`
    - `Sequence(is_step_0 -> run_leave_gang_to_difu)`
    - `Sequence(is_step_1 -> run_difu_buy_paper)`
    - `Sequence(is_step_2 -> run_travel_to_beiju)`
    - `Sequence(is_step_3 -> run_beiju_sell_paper_buy_oil)`
    - `Sequence(is_step_4 -> run_return_to_difu)`
    - `Sequence(is_step_5 -> run_difu_sell_oil)`
    - `Action(cycle_restart_if_needed)`

说明：

- 每个 `tick` 最多推进一个业务步骤，`next_op` 作为分发条件
- `goal_branch` 优先级高于普通步骤分支（达标后立即回帮提交）
- `cycle_restart_if_needed` 仅在 `next_op == steps.size()` 时触发新一轮 cycle
- 长耗时动作采用“异步启动 + Running 轮询”实现，`tick()` 本身保持非阻塞

收益：

- 业务步骤可插拔
- checkpoint 推进逻辑统一
- 控制面（STOP/STATUS）响应更及时
- goal 分支与普通执行分支解耦

## 4. 可靠性基线

- 控制线程与执行线程分离（条件变量驱动，避免轮询）
- 所有异常必须落日志并转成可观察状态
- checkpoint 在 stop/步骤推进/关键状态变化时持久化
- 远程命令幂等（重复 INIT/START 不导致状态错乱）

## 5. 可观测性基线

必须输出：

- 生命周期状态变化日志
- 命令请求与响应（至少文本级）
- 初始化失败原因（`INIT_FAILED <reason>`）

建议后续补充：

- 结构化 JSON 日志
- Prometheus 指标（状态、循环次数、失败次数、命令耗时）

## 6. 安全与运行规范

- 控制端口默认仅内网可达
- WebUI 与控制端建议通过网段 ACL/VPN 隔离
- 禁止在未初始化状态下直接执行 run

## 7. 后续工业化改造清单

- 抽离 `RuntimeController` 为独立类（`src/runtime/`）[done]
- 抽离行为树节点到独立模块（`src/bt/`）[done]
- 跑商主循环改为显式 step 分发行为树（非单一 step_action）[done]
- step 分发分支构建器抽离为 `src/bt/StepDispatchTree.*` [done]
- 命令协议升级为 JSON（带 request_id）[in progress: text protocol already supports request_id]
- 增加集成测试（命令状态转换、checkpoint 恢复、异常恢复）
- 增加故障注入测试（初始化失败、网络抖动、识别失败）
