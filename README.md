# 梦幻西游跑商机器人

基于 Hypervisor 内存读取 + DXGI 屏幕截图 + OpenCV 模板匹配的全自动跑商工具。

---

## 功能概览

| 功能 | 说明 |
|------|------|
| 🗺️ 自动路线 | 地府 ↔ 北俱泸州 8 段传送全自动导航 |
| 💰 智能比价 | 自动对比两个商人的买入/卖出价，选择最优 |
| 🔄 断点续跑 | JSON checkpoint 保存进度，中断后自动从上次位置继续 |
| 🎯 目标达标 | 银票达到目标金额后自动返回帮派并停止 |
| 🖱️ 拟人操作 | 贝塞尔曲线鼠标轨迹 + 随机延迟，模拟人类操作 |
| 🔍 NPC 识别 | OpenCV 模板匹配定位 NPC、按钮和物品图标 |
| 📊 统一日志 | 带时间戳和模块名的结构化日志输出 |

## 跑商路线

```
帮派 → 长安城 → 大唐国境 → 地府
                                ↓
        地府买纸钱（比价两个商人）
                                ↓
地府 → 大唐国境 → 赤水洲 → 女魃墓 → 东海岩洞 → 东海湾 → 傲来国 → 花果山 → 北俱泸州
                                                                                ↓
                                                        北俱卖纸钱（比价）+ 买油
                                                                                ↓
北俱泸州 → 长安城 → 大唐国境 → 地府
                                ↓
                地府卖油（比价），达标则回帮派
                                ↓
                    未达标 → 循环继续
```

---

## 项目结构

```
├── main.cpp                  # 入口：参数解析 + HV 初始化 + 运行循环
├── hv.h / hv.asm             # Hypervisor 驱动接口（内存读写、进程查询）
├── dumper.h / dumper.cpp      # 内存 dump 工具
├── src/
│   ├── BotLogger.h            # 统一日志宏（BOT_LOG / BOT_WARN / BOT_ERR）
│   ├── GameMemory.h / .cpp    # 内存读取层：CR3 管理 + 角色坐标读取/换算
│   ├── VisionEngine.h / .cpp  # 视觉引擎：截图 + 地图识别 + NPC 查找
│   ├── TencentBot.h            # 主类声明
│   ├── TencentBot.Core.cpp     # 初始化/生命周期
│   ├── TencentBot.Motion.cpp   # 输入控制/寻路/传送
│   ├── TencentBot.Routes.cpp   # 路线段与恢复路线
│   ├── TencentBot.TradeOps.cpp # 交易读写与买卖流程
│   ├── TencentBot.TradingRoute.cpp # 跑商行为树编排
│   ├── TencentBot.Captcha.cpp  # 银票提交成语验证
│   ├── DxgiWindowCapture.h/.cpp  # DXGI 屏幕截图
│   ├── CaptchaEngine.h / .cpp # AI 文字/数字识别（HTTP 客户端 → 本地服务）
│   ├── CheckpointStore.h/.cpp # JSON 断点存储
│   ├── config/BotSettings.h/.cpp # 运行参数配置加载
│   └── domain/MapProperties.h # 地图参数结构
├── assets/
│   ├── maps/                  # 地图识别模板（12 张 .png）
│   ├── mapsui/                # 小地图 UI 模板（11 张，含昼夜变体）
│   ├── npc/                   # NPC / 按钮 / 物品模板（20 张 .png）
│   └── config/bot_settings.json # 可配置参数（时间/阈值/地图参数）
├── IbInputSimulator/          # 鼠标/键盘模拟器（Logitech 驱动）
├── CMakeLists.txt             # CMake 构建配置
└── vcpkg.json                 # vcpkg 依赖清单
```

---

## 架构设计

项目采用**分层解耦**架构，每层职责单一：

```
┌─────────────────────────────────────────┐
│            main.cpp（入口）              │
│   参数解析 / HV 初始化 / 信号处理        │
└─────────────┬───────────────────────────┘
              │
┌─────────────▼───────────────────────────┐
│         TencentBot（调度层）              │
│   路线导航 / 交易逻辑 / 断点续跑          │
├──────┬──────────┬──────────┬────────────┤
│      │          │          │            │
│ GameMemory  VisionEngine  CaptchaEngine │
│ 内存读取     视觉识别       AI 识别      │
│      │          │                       │
│   hv.h    DxgiWindowCapture             │
│  HV 驱动    DXGI 截图                   │
└─────────────────────────────────────────┘
```

### 各模块说明

#### BotLogger（`BotLogger.h`）

头文件式日志系统，提供三个宏：

```cpp
BOT_LOG("Module", "消息内容 " << variable);   // [INFO]  正常日志
BOT_WARN("Module", "警告内容");               // [WARN]  警告信息
BOT_ERR("Module", "错误内容");                // [ERROR] 错误信息
```

输出格式：`[2026-02-19 16:30:00] [INFO ] [Module    ] 消息内容`

#### GameMemory（`GameMemory.h/.cpp`）

封装 Hypervisor 内存访问和游戏坐标系统：

- **`readPitPosRaw(processIndex)`** — 读取角色原始像素坐标（二级指针解引用）
- **`readRoleGameCoord(processIndex, mapName)`** — 将原始坐标换算为游戏世界坐标
- 持有多进程的 PID / CR3 / DLL 基地址数组（支持多开）

坐标换算公式：
```
GameCoord.x = (rawX - 10) / 20
GameCoord.y = (kMapYBase[mapName] - rawY) / 20
```

#### VisionEngine（`VisionEngine.h/.cpp`）

基于 OpenCV `matchTemplate` 的视觉识别引擎：

- **`getCurrentMapName()`** — 截图 + 遍历 12 张地图模板匹配，返回当前地图名
- **`locateMapUiOnScreen(mapName)`** — 定位小地图 UI 控件位置（支持昼夜双模板）
- **`findNpcOnScreen(npcName)`** — 全屏查找 NPC，返回中心坐标
- **`findNpcInScreenRegion(...)`** — 在指定矩形区域内查找 NPC

模板文件在编译后自动复制到输出目录（`maps/`、`npcs/`、`mapsui/`）。

#### TencentBot（`TencentBot.*.cpp`）

高层调度器，不直接接触底层 API：

- **路线函数** — 每个 `route_*()` 对应一次地图间传送
- **交易函数** — `buyItemFromNpc()` / `sellItemToNpc()` 封装完整买卖流程
- **比价逻辑** — `queryNpcBuyPrice()` / `queryNpcSalePrice()` 自动选择便宜商人
- **`runTradingRoute()`** — 6 步操作循环，含 checkpoint 和达标检测

#### CheckpointStore（`CheckpointStore.h/.cpp`）

JSON 文件持久化，支持断点续跑：

```json
{
  "version": 3,
  "route": "TradingRoute",
  "next_op": 2,
  "last_op_name": "difu_buy_paper",
  "cycle": 0,
  "target_money": 150000
}
```

`next_op` 对应 6 个操作步骤：

| next_op | 操作名 | 说明 |
|---------|--------|------|
| 0 | `leave_gang_to_difu` | 出帮派 → 长安 → 大唐国境 → 地府 |
| 1 | `difu_buy_paper` | 地府买纸钱（比价） |
| 2 | `travel_to_beiju` | 地府 → …8段… → 北俱泸州 |
| 3 | `beiju_sell_paper_buy_oil` | 北俱卖纸钱 + 买油 |
| 4 | `return_to_difu` | 北俱 → 长安 → 大唐国境 → 地府 |
| 5 | `difu_sell_oil` | 地府卖油（达标则回帮派） |

---

## 依赖项

| 依赖 | 版本 | 用途 |
|------|------|------|
| OpenCV | ≥ 4.12.0 | 图像模板匹配 |
| nlohmann-json | ≥ 3.12.0 | Checkpoint JSON 序列化 |
| curl | ≥ 8.18.0 | CaptchaEngine HTTP 请求 |
| cppwinrt | ≥ 2.0 | Windows Runtime（DXGI 截图） |
| detours | ≥ 4.0.1 | API Hook |
| IbInputSimulator | 子模块 | Logitech 鼠标/键盘模拟 |

## 构建

### 环境要求

- Windows 10/11（SDK ≥ 10.0.19041.0）
- Visual Studio 2022（MSVC v143，支持 C++20）
- vcpkg（集成模式）
- 自定义 Hypervisor 驱动已加载

### 编译步骤

```powershell
# 1. 安装 vcpkg 依赖
vcpkg install

# 2. CMake 配置（指定 vcpkg 工具链）
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="vcpkg/scripts/buildsystems/vcpkg.cmake"

# 3. 编译
cmake --build build --config Release
```

### 输出

编译后 `build/Release/` 目录包含：
- `untitled.exe` — 主程序
- `IbInputSimulator.dll` — 输入模拟器
- `maps/`、`npcs/`、`mapsui/` — 模板图片（自动复制）
- `config/bot_settings.json` — 运行配置文件（自动复制）

---

## 运行

### 前提条件

1. Hypervisor 驱动已加载（`hv::is_hv_running()` 返回 true）
2. 游戏进程 `mhmain.exe` 和 `mhtab.exe` 正在运行
3. Host 侧内存后端已启动（推荐 C++ 后端，监听 `4050`）
4. AI 识别服务运行在 `http://127.0.0.1:8000`
5. 屏幕分辨率 2560×1440（逻辑分辨率）

### 启动内存后端（推荐）

```bash
# Host: C++ 后端（推荐）
cd /root/workspace/TencentBot-vmi-mem-backend-cpp
cmake --preset linux-release
cmake --build --preset build-release
./build/release/tencentbot_mem_backend --transport vsock --listen-cid 2 --listen-port 4050 --backend mock --verbose
```

生产环境建议把 `--backend mock` 替换为 `--backend command --tool <你的物理读写工具>`。
例如：

```bash
./build/release/tencentbot_mem_backend --transport vsock --listen-cid 2 --listen-port 4050 --backend command --tool /root/workspace/TencentBot-vmi-mem-backend/run_memflow_tool.sh
```

### 启动内存后端（备选：Python）

```bash
cd /root/workspace/TencentBot-vmi-mem-backend
python3 backend.py --transport vsock --listen-cid 2 --listen-port 4050 --backend memflow --memflow-os qemu --memflow-os-args vm101
```

### 基本用法

```powershell
# 正常运行（默认目标 150000 银票）
.\untitled.exe

# 查看当前 checkpoint
.\untitled.exe --show-checkpoint

# 重置 checkpoint（从头开始）
.\untitled.exe --reset-checkpoint

# 设置目标银票
.\untitled.exe --set-target-money 200000

# 设置从第 2 步（travel_to_beiju）开始
.\untitled.exe --set-next-op 2

# 指定 checkpoint 文件路径
.\untitled.exe --checkpoint mybot.json

# 使用 vsock INIT_BIND（连接 Host 独立内存后端）
.\untitled.exe --mem-backend vsock --cid 2 --port 4050 --vsock-timeout-ms 5000
```

### 安全停止

- **Ctrl+C** 或 **关闭控制台窗口** — 保存 checkpoint 后安全退出
- **回车键** — 同上，请求停止
- 下次运行自动从上次中断位置继续

---

## 添加新 NPC 模板

1. 截取 NPC 图标，保存为 `assets/npc/名称.png`
2. 在 `VisionEngine.cpp` 的 `initNpcConfigs()` 中添加一行：
   ```cpp
   npcConfigs["显示名"] = {"npcs/名称.png", 0.7};
   ```
3. 重新编译，模板图片会自动复制到输出目录

## 添加新地图

1. 截取地图特征区域，保存为 `assets/maps/地图名.png`
2. 截取小地图 UI，保存为 `assets/mapsui/地图名.png`（如有夜间版另存 `地图名1.png`）
3. 在 `assets/config/bot_settings.json` 的 `map_properties` 中添加条目：
   ```cpp
   {"地图名", {UI宽度, UI高度, 游戏坐标X最大值, 游戏坐标Y最大值}},
   ```
4. 在 `GameMemory.cpp` 的 `kMapYBase` 中添加 Y 轴基准值：
   ```cpp
   {"地图名", Y轴基准值},
   ```

---

## 许可

仅供学习研究使用，请勿用于违反游戏服务条款的行为。
