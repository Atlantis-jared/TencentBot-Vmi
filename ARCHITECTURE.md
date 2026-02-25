# 架构详解

本文档面向开发者，详细说明项目的技术实现细节。

---

## 数据流

```
┌──────────────────────── 游戏进程 ────────────────────────┐
│  mhmain.exe（游戏逻辑）    mhtab.exe（UI 渲染窗口）       │
└──────┬────────────────────────────────┬──────────────────┘
       │ ① hv::read_virt_mem            │ ② DXGI Desktop Dup
       │    读取角色坐标内存              │    截取游戏画面
       ▼                                ▼
 ┌──────────┐                  ┌──────────────────┐
 │GameMemory│                  │DxgiWindowCapture │
 │ RawCoord │                  │ BGRA imageBuffer │
 │ GameCoord│                  └────────┬─────────┘
 └────┬─────┘                           │
      │                                 ▼
      │                        ┌──────────────┐
      │                        │ VisionEngine │
      │                        │ matchTemplate│ ←── 模板 .png
      │                        │ 地图/NPC 识别 │
      │                        └────────┬─────┘
      │                                 │
      ▼                                 ▼
 ┌──────────────────────────────────────────┐
 │              TencentBot                  │
 │  路线调度 → 小地图点击 → 交易操作         │
 │  ↕ IbInputSimulator（鼠标/键盘模拟）     │
 │  ↕ CaptchaEngine（价格数字 OCR）         │
 │  ↕ CheckpointStore（JSON 断点）          │
 └──────────────────────────────────────────┘
```

---

## 内存读取原理（GameMemory）

### 指针链

```
DLL基址 + 0x26A0C88  →  一级指针 (8字节)
一级指针 + 0x370      →  角色X坐标 (uint32)
一级指针 + 0x374      →  角色Y坐标 (uint32)
```

所有读取通过 `hv::read_virt_mem(cr3, dst, src, size)` 完成，无需注入目标进程。

### 坐标换算

游戏使用的原始坐标是高精度像素值，需要换算为人类可读的「地图格子坐标」：

```
GameCoord.x = (rawPixelX - 10) / 20
GameCoord.y = (kMapYBase[当前地图名] - rawPixelY) / 20
```

`kMapYBase` 是每张地图独立的 Y 轴基准值（因为不同地图的原点位置不同）。

---

## 视觉识别原理（VisionEngine）

### 模板匹配流程

```
截图(BGRA) → cvtColor(BGR) → matchTemplate(TM_CCOEFF_NORMED) → minMaxLoc → 分数判定
```

- **地图识别**：遍历 12 张地图模板，取最高分（阈值 ≥ 0.80）
- **NPC 识别**：单一模板匹配（阈值 ≥ 0.70），返回中心坐标
- **UI 定位**：日/夜双模板策略（文件名后缀 `1`），阈值 ≥ 0.75

### 模板缓存策略

| 模板类型 | 加载时机 | 缓存位置 |
|---------|---------|---------|
| 地图模板 | `loadAllTemplates()` | `mapTemplates` |
| NPC 模板 | `loadAllTemplates()` | `npcTemplates` |
| UI 模板 | 首次 `locateMapUiOnScreen()` | `mapUiTemplateCache`（懒加载） |

---

## 鼠标轨迹算法（TencentBot）

使用**三次贝塞尔曲线 + 过冲拉回**两阶段策略：

### 阶段1：贝塞尔曲线

```
B(t) = (1-t)³·P₀ + 3(1-t)²t·P₁ + 3(1-t)t²·P₂ + t³·P₃
```

- P₀ = 当前位置
- P₁, P₂ = 在法线方向随机偏移的控制点（产生弧线效果）
- P₃ = 目标 + 过冲量（1.5~3.0 像素单位）
- 使用 ease-out 缓动函数 `t' = t(2-t)`

### 阶段2：精确拉回

从过冲点微调到精确目标，步长系数 0.25，最多 15 次迭代，到达半径 2.0。

---

## 断点续跑机制

### 数据结构

```json
{
  "version": 3,
  "route": "TradingRoute",
  "next_op": 0,
  "last_op_name": "",
  "updated_at_ms": 1740000000000,
  "cycle": 0,
  "target_money": 150000
}
```

### 恢复策略

- `next_op=0`（出帮派→地府）：调用 `resumeRoute_leaveBangpaiToDifu()`，根据当前地图自动判断从哪个节点继续
- `next_op=2`（地→北俱）：调用 `resumeRoute_travelToBeixu()`，8 段传送链路中任意点恢复
- 其他步骤：直接从对应操作开始执行

### 版本迁移

启动时自动将旧版 checkpoint 升级到 v3 格式（调整 `next_op` 偏移并设置版本号）。

---

## 交易逻辑

### 比价流程

```
走到商人A → 查询价格 → 关闭面板
走到商人B → 查询价格 → 关闭面板
选价格更优的商人 → 执行买入/卖出
```

### 价格识别

1. 打开交易面板
2. 通过 AI 检测（`CaptchaEngine.detectObject`）或模板匹配定位物品图标
3. 点击物品后截图目标价格区域（固定 ROI）
4. 将 ROI 图像发送给 AI 服务识别数字（`CaptchaEngine.recognizeNumber`）

### 卖出验证

卖出操作后在物品原位置做二次模板匹配：
- 物品图标消失 → 判定成功
- 图标仍在 → 判定失败，自动重试（最多 2 次）

---

## 传送重试机制

NPC 传送和 UI 传送均支持自动重试（默认 3 次）：

```
尝试传送 → 检查地图是否切换成功
  ↓ 失败
隐藏其他玩家 → 走回 NPC 附近 → 重新尝试
  ↓ 再次失败
重复直到达到最大尝试次数
```

失败原因通常是其他玩家模型遮挡 NPC 识别。`hideOtherPlayers()` 通过 Alt+H + F9 组合键清理视野。

---

## 关键常量

| 常量 | 值 | 说明 |
|------|-----|------|
| `GAME_PIT_CHAIN_BASE_OFFSET` | 0x26A0C88 | DLL 基址 + 此偏移 = 一级指针 |
| `GAME_PIT_POS_STRUCT_OFFSET` | 0x370 | 一级指针 + 此偏移 = X 坐标 |
| `kMapRecognizeMinScore` | 0.80 | 地图识别最低分数 |
| `kMapUiLocateMinScore` | 0.75 | UI 定位最低分数 |
| `WINDOW_BORDER_OFFSET_X/Y` | -6, -57 | 窗口边框补偿 |
| `MOVE_SCALE` | 0.35 | 游戏坐标差→鼠标位移比例 |
