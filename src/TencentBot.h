//
// TencentBot.h — 跑商机器人主类（高层路线与交易调度）
//
// 职责划分（解耦后）：
//   - 内存读取 → GameMemory（src/GameMemory.h）
//   - 视觉识别 → VisionEngine（src/VisionEngine.h）
//   - 屏幕截图 → DxgiWindowCapture（src/DxgiWindowCapture.h）
//   - AI 识字  → CaptchaEngine（src/CaptchaEngine.h）
//   - 日志输出 → BotLogger（src/BotLogger.h）
//   - TencentBot 只负责：
//       1. 初始化各子系统
//       2. 实现游戏内路线（长安→地府→北俱 等）
//       3. 实现交易逻辑（价格查询、买卖操作）
//       4. 运行完整跑商循环 runTradingRoute()
//

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <functional>

#include "BotLogger.h"
#include "CaptchaEngine.h"
#include "DxgiWindowCapture.h"
#include "GameMemory.h"
#include "VisionEngine.h"
#include "../IbInputSimulator/Simulator/include/IbInputSimulator/InputSimulator.hpp"

// =============================================================================
// MapProperties — 描述一张地图 UI 控件的物理尺寸与游戏坐标范围
// 用于在小地图上把 (gameX, gameY) 换算为屏幕像素坐标并点击。
// =============================================================================
struct MapProperties {
    int uiPixelWidth;   // 小地图 UI 的像素宽度（从模板图获得）
    int uiPixelHeight;  // 小地图 UI 的像素高度
    int gameCoordMaxX;  // 地图游戏坐标 X 轴最大值
    int gameCoordMaxY;  // 地图游戏坐标 Y 轴最大值
};

// =============================================================================
// TencentBot — 跑商机器人主类
// =============================================================================
class TencentBot {
public:
    TencentBot();
    ~TencentBot();

    // -------------------------------------------------------------------------
    // init() — 初始化所有子系统
    // 执行顺序：
    //   1. 遍历进程列表，找到 mhmain.exe（游戏主进程）和 mhtab.exe（UI 进程）
    //   2. 通过 hv 获取每个游戏进程的 CR3 和 mhmain.dll 基地址，注入 GameMemory
    //   3. 初始化鼠标模拟器（Logitech 驱动）
    //   4. 初始化 CaptchaEngine（连接本地 AI 服务 http://127.0.0.1:8000）
    //   5. 初始化 DxgiWindowCapture（根据 mhtab.exe PID 找到游戏窗口句柄）
    //   6. 加载地图和 NPC 模板（VisionEngine）
    // -------------------------------------------------------------------------
    void init();

    // -------------------------------------------------------------------------
    // configureRunControl() — 配置中断和断点续跑参数
    // 参数：
    //   stopSignal     — 指向原子布尔的指针，main 线程会在 Ctrl+C 时置为 true
    //   checkpointFile — checkpoint JSON 文件路径（默认 bot_checkpoint.json）
    // 说明：
    //   每个耗时操作前都会检查 stopSignal，收到停止信号则保存 checkpoint 并退出。
    // -------------------------------------------------------------------------
    void configureRunControl(std::atomic_bool* stopSignal,
                             const std::string& checkpointFile = "bot_checkpoint.json");

    // -------------------------------------------------------------------------
    // runTradingRoute() — 完整跑商循环（阻塞，直至达标/手动停止/异常）
    // 流程：
    //   [1/6] 出帮派 → 长安 → 大唐国境 → 地府
    //   [2/6] 地府买纸钱（比价后选便宜的商人）
    //   [3/6] 地府 → 大唐国境 → 赤水洲 → … → 北俱泸州（8段传送）
    //   [4/6] 北俱卖纸钱（比价）+ 买油
    //   [5/6] 北俱 → 长安 → 大唐国境 → 地府
    //   [6/6] 地府卖油（比价），达标则回帮派并清除 checkpoint
    // 支持断点续跑：每步完成后保存 checkpoint，下次从中断点继续。
    // -------------------------------------------------------------------------
    void runTradingRoute();

    void process_idiom_verify();

    // =========================================================================
    // 公开子系统实例（main.cpp 需要访问 gameMemory 填充进程信息）
    // =========================================================================
    GameMemory      gameMemory;    // 内存读取层
    DxgiWindowCapture screenCapture; // DXGI 屏幕截图

private:
    // =========================================================================
    // 子系统实例
    // =========================================================================
    VisionEngine              vision;        // 视觉识别引擎（持有 screenCapture 的引用）
    std::unique_ptr<CaptchaEngine> aiCaptcha; // 验证码 / 文字识别（HTTP 客户端）

    // =========================================================================
    // 运行控制
    // =========================================================================
    std::atomic_bool* stopSignal_     = nullptr;           // 中断信号（来自 main 线程）
    std::string       checkpointFile_ = "bot_checkpoint.json"; // checkpoint 文件路径

    // =========================================================================
    // 底层输入操作（鼠标移动 / 点击）
    // =========================================================================

    // -------------------------------------------------------------------------
    // moveCharacterTo() — 将角色移动到游戏坐标 (targetX, targetY)
    // 通过读取角色实时坐标 + 贝塞尔曲线插值生成鼠标轨迹来实现。
    // 参数：
    //   processIndex — 操作哪个游戏进程（多开时使用）
    // -------------------------------------------------------------------------
    void moveCharacterTo(int targetX, int targetY, int processIndex);

    // -------------------------------------------------------------------------
    // clickMapPosition() — 在小地图上点击目标游戏坐标（调用地图 UI 定位换算）
    // 参数：
    //   mapName  — 目标地图名（用于查找 MapProperties 和定位 UI）
    //   gameX/Y  — 目标游戏坐标
    //   processIndex — 当前控制的进程编号
    // -------------------------------------------------------------------------
    void clickMapPosition(const std::string& mapName, int gameX, int gameY, int processIndex);

    // -------------------------------------------------------------------------
    // moveCharacterToOffset() — 移动角色到目标坐标 + 自定义偏移
    // 封装 moveCharacterTo，适用于需要停在目标左边/右边的场景。
    // -------------------------------------------------------------------------
    void moveCharacterToOffset(int targetX, int targetY, int processIndex,
                               int extraOffsetX, int extraOffsetY);

    // -------------------------------------------------------------------------
    // clickUiElement() — 点击屏幕上的指定像素坐标（UI 按钮、对话框等）
    // 内部先调 moveCharacterToOffset 移动鼠标，再发送左键点击，再等待 UI 刷新。
    // 参数：
    //   screenX/Y  — 屏幕像素坐标（基于游戏窗口内部）
    //   extraOffX/Y — 额外偏移（可用于微调点击点，默认为 0）
    // -------------------------------------------------------------------------
    void clickUiElement(int screenX, int screenY, int extraOffX = 0, int extraOffY = 0);

    // -------------------------------------------------------------------------
    // clickNpcIfFound() — 识别并点击指定 NPC
    // 返回：true = 找到并点击；false = 未找到
    // 参数：
    //   npcName — NPC 名称（需在 VisionEngine 中有模板）
    //   clickOffsetX/Y — 点击坐标相对于 NPC 中心的偏移（用于避开名字遮挡等）
    // -------------------------------------------------------------------------
    bool clickNpcIfFound(const std::string& npcName, int clickOffsetX = 0, int clickOffsetY = 0);

    // -------------------------------------------------------------------------
    // hideOtherPlayers() — 按 Alt+H 隐藏其他玩家，F9 刷新视野
    // 目的：减少其他玩家模型对 NPC 识别的干扰。
    // -------------------------------------------------------------------------
    void hideOtherPlayers();

    // =========================================================================
    // 导航辅助
    // =========================================================================

    // -------------------------------------------------------------------------
    // walkToPosition() — 打开小地图，点击目标坐标，等待角色到达
    // 内部：Tab开图 → clickMapPosition → Tab关图 → 轮询坐标直到到达或超时
    // -------------------------------------------------------------------------
    void walkToPosition(const std::string& mapName, int targetX, int targetY);

    // -------------------------------------------------------------------------
    // tryNpcTeleport() — 通过点击 NPC 弹出传送选项来传送
    // 失败重试时，先用 walkToPosition 重新走到 NPC 附近再尝试。
    // 参数：
    //   npcName        — NPC 名称（VisionEngine 识别）
    //   npcClickOffX/Y — 点击 NPC 时相对于 NPC 中心的偏移
    //   menuClickX/Y   — 传送菜单中"传送"按钮的屏幕坐标
    //   expectedMap    — 传送成功后应到达的地图名
    //   retryWalkMap   — 重试时先走到该地图的某位置
    //   retryWalkX/Y   — 重试时走到的坐标
    //   maxRetries     — 最大尝试次数（默认 3）
    // -------------------------------------------------------------------------
    bool tryNpcTeleport(const std::string& npcName,
                        int npcClickOffX, int npcClickOffY,
                        int menuClickX, int menuClickY,
                        const std::string& expectedMap,
                        const std::string& retryWalkMap,
                        int retryWalkX, int retryWalkY,
                        int maxRetries = 3);

    // -------------------------------------------------------------------------
    // tryUiTeleport() — 直接点击 UI 坐标传送（用于不需要识别 NPC 的入口）
    // 参数：
    //   clickX/Y      — 传送按钮的屏幕坐标
    //   extraOffX/Y   — 点击偏移
    //   expectedMap   — 期望到达的地图名
    //   retryWalkMap/X/Y — 重试时走到的位置
    //   maxRetries     — 最大尝试次数
    // -------------------------------------------------------------------------
    bool tryUiTeleport(int clickX, int clickY,
                       int extraOffX, int extraOffY,
                       const std::string& expectedMap,
                       const std::string& retryWalkMap,
                       int retryWalkX, int retryWalkY,
                       int maxRetries = 3);

    // =========================================================================
    // 路线段函数（每段对应一次地图间的传送）
    // =========================================================================
    void route_changan_to_datangguojing();       // 长安城 → 大唐国境（驿站老板传送）
    void route_datangguojing_to_difu();          // 大唐国境 → 地府（UI 传送门）
    void route_leaveDisfu();                     // 地府 → 大唐国境（出地府入口）
    void route_datangguojing_to_chishuizhou();   // 大唐国境 → 赤水洲（守卫传送）
    void route_datangguojing_to_changancheng();

    void route_datangguojing_to_changan();       // 大唐国境 → 长安城（驿站老板）
    void route_chishuizhou_to_nvbamu();          // 赤水洲 → 女魃墓（UI 传送）
    void route_nvbamu_to_donghaiyandong();       // 女魃墓 → 东海岩洞（UI 传送）
    void route_donghaiyandong_to_donghaiwan();   // 东海岩洞 → 东海湾（UI 传送）
    void route_donghaiwan_to_aolaiguo();         // 东海湾 → 傲来国（传送傲来 NPC）
    void route_aolaiguo_to_huaguoshan();         // 傲来国 → 花果山（UI 传送）
    void route_huaguoshan_to_beijuluzhou();      // 花果山 → 北俱泸州（传送北俱 NPC）
    void route_beijuluzhou_to_changan();         // 北俱泸州 → 长安城（驿站老板）
    void route_changan_to_bangpai();             // 长安城 → 帮派（帮派主管）
    void route_leaveBangpai();                   // 帮派 → 长安城（出门按钮序列）

    // =========================================================================
    // 断点续跑恢复路线（综合识别当前地图，从任意中途节点恢复）
    // =========================================================================

    // -------------------------------------------------------------------------
    // resumeRoute_leaveBangpaiToDifu() — 从帮派/长安/大唐国境任意点恢复到地府
    // 用于 runTradingRoute op0 断线后续跑。
    // -------------------------------------------------------------------------
    void resumeRoute_leaveBangpaiToDifu();

    // -------------------------------------------------------------------------
    // resumeRoute_travelToBeixu() — 从地府→…→北俱整条链路的任意地图恢复
    // 用于 runTradingRoute op2 断线后续跑。
    // -------------------------------------------------------------------------
    void resumeRoute_travelToBeixu();

    // =========================================================================
    // 走到地图内商人附近的快捷函数（配合交易逻辑使用）
    // =========================================================================
    void walkToDifuLowerMerchant();   // 地府：走到"下商人"（地府商人）附近
    void walkToDifuUpperMerchant();   // 地府：走到"上商人"（地府货商）附近
    void walkToBeixuUpperMerchant();  // 北俱：走到"上商人"（北俱货商）附近
    void walkToBeixuLowerMerchant();  // 北俱：走到"下商人"（北俱商人）附近

    // =========================================================================
    // 交易面板操作
    // =========================================================================

    // -------------------------------------------------------------------------
    // readItemPrice() — 打开 NPC 交易面板，识别指定物品的买入或卖出价格
    // 参数：
    //   tradeMode — 0=买入价（左侧面板），1=卖出价（右侧面板）
    //   itemName  — 物品名称（用于 CaptchaEngine / 模板匹配定位物品图标）
    // 返回：
    //   价格整数；-1=物品未找到，-2=坐标越界，-3=数字识别失败
    // -------------------------------------------------------------------------
    int readItemPrice(int tradeMode, const std::string& itemName);

    // -------------------------------------------------------------------------
    // readCurrentMoney() — 读取当前银票总额（需交易面板已打开且鼠标不遮挡）
    // 返回：银票数量；-1=截图失败，-2=ROI 越界，-3=数字识别失败，-4=其他
    // -------------------------------------------------------------------------
    int readCurrentMoney();
    bool waitForTradePanel(); // 等待交易面板打开

    // -------------------------------------------------------------------------
    // buyItemFromNpc() — 点击 NPC → 打开买入面板 → 找物品 → 最大数量 → 确定
    // 返回：操作是否成功（false = NPC 或物品未找到）
    // -------------------------------------------------------------------------
    bool buyItemFromNpc(const std::string& npcName, const std::string& itemName);

    // -------------------------------------------------------------------------
    // sellItemToNpc() — 点击 NPC → 打开卖出面板 → 找物品 → 最大数量 → 确定 → 验证物品消失
    // 最多重试 2 次，通过"卖出后物品图标是否消失"来判断是否成功。
    // 改进：卖出成功后在面板关闭前直接读取银票总额，省去重新点击 NPC 的步骤。
    // 返回：卖出成功时返回当前银票总额（≥0）；卖出失败返回 -1
    // -------------------------------------------------------------------------
    int sellItemToNpc(const std::string& npcName, const std::string& itemName);

    // -------------------------------------------------------------------------
    // queryNpcBuyPrice() — 走到 NPC 附近，查询买入价后关闭面板
    // -------------------------------------------------------------------------
    int queryNpcBuyPrice(const std::string& npcName, const std::string& itemName);

    // -------------------------------------------------------------------------
    // queryNpcSalePrice() — 走到 NPC 附近，查询卖出价后关闭面板
    // -------------------------------------------------------------------------
    int queryNpcSalePrice(const std::string& npcName, const std::string& itemName);
};
