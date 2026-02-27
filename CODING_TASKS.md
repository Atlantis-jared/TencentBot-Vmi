# Host-Guest 零延迟物理内存注入架构重构 — 任务清单

## 架构说明
本文件由 **架构 Agent** 规划，**编码 Agent** 负责实现。
当前目标：废弃低效的纯 vsock 内存读取方案，改为“vsock 初始化握手 + 宿主机物理内存暴力覆盖 (Direct Memory Overwrite)”的极致性能方案。

详情参考同级目录下的架构文档：`ARCHITECTURE_DESIGN.md`

## Phase 1: 空间清理与架构框定 (架构 Agent 已完成)
- [x] 删除历史遗留的 `guestread`, `memflow`, `libvmi` 等臃肿的试验性目录。
- [x] 确立极简架构：只保留 `TencentBot-vmi` (Guest 跑商逻辑) 和 `TencentBot-vmi-webui` (Host 控制与底层内存强写引擎) 两个核心仓库。

---
⬇️ **以下任务请 编码 Agent 执行实现** ⬇️

## Phase 2: Guest 端 (TencentBot-vmi 仓库) 改造
- [x] 1. **结构体植入**：已新增 `src/SharedDataStatus.h/.cpp`，使用 `volatile` + `#pragma pack(push, 1)` + 全局 `g_shared_data`。
- [x] 2. **握手通讯**：已重构 `MemoryReader.cpp` 的 vsock 逻辑为一次性 `INIT_BIND` 握手，携带 `bot_pid` + `bot_receive_addr` + 目标进程信息。
- [x] 3. **拆除旧网络读写**：`VsockMemoryReader` 已移除旧的高频读包流程，改为仅保留一次性 `INIT_BIND` 握手。
- [x] 4. **业务层适配**：`GameMemory` 已改为优先读取 `SharedDataStatus`，并在 `--print-cursor` 调试模式输出共享结构体实时值。

## Phase 3: Host 端改造（独立后端项目）
- [x] 1. **核心底座迁移与增强**：已新建独立项目 `../TencentBot-vmi-mem-backend-cpp`，并通过 `../TencentBot-vmi-memflow-rs`（官方 `memflow` Rust crate）提供真实读写链路。
- [x] 2. **握手管道监听**：`TencentBot-vmi-mem-backend-cpp/src/main.cpp` 已实现 `INIT_BIND` 监听（支持 vsock/tcp），收到 Guest 虚拟地址后建立 worker 上下文。
- [x] 3. **暴写循环引擎**：已实现高频注入线程（`--tick-ms` 默认 10ms），循环执行：
      `从游戏进程地址脱壳读出最新坐标 => 构造带时间戳和 FLAG 的 Payload => 瞬间写入 Guest Bot 共享结构体`。
- [x] 4. **初始化读探针校验**：`INIT_BIND` 新增 `bot_probe_addr/bot_probe_value`，Host 回读 `probe_read_value` 并由 Guest 严格比对，确认“后端确实能读到 Guest 内存”。

## Phase 4: 编译与联调测试 (最终环节)
- [ ] 1. 在 Windows 编译全新的 `TencentBot-vmi.exe` 并在虚拟机中启动。
- [ ] 2. 在 Linux PVE 环境拉起独立注入守护进程（C++）：`TencentBot-vmi-mem-backend-cpp/tencentbot_mem_backend`。
- [ ] 3. 核对心跳同步标志 `sync_flag` 从 0 变 1，确认无网络包的数据闭环跑通。
