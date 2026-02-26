#pragma once
//
// TradingStepTrees.h — 跑商行为树子树构建器
//
// 将 runTradingRoute() 中 6 个 step 的业务逻辑拆分为独立的行为树子树。
// 每个 BuildStepX() 返回一棵子树（unique_ptr<bt::Node>），内部所有子步骤
// 都是可观测的 BT 节点，不再使用手写 phase 状态机。
//
// 设计原则：
//   1. 通过 TradingStepContext 回调注入所有外部依赖，避免直接耦合 TencentBot
//   2. 子树内部自带状态（通过 shared_ptr 或 Action 闭包捕获）
//   3. Observer 透传到每个子节点，保证全链路可观测
//

#include "../bt/BehaviorTree.h"
#include "../CheckpointStore.h"
#include "../config/BotSettings.h"

#include <functional>
#include <memory>
#include <string>

namespace domain {

// ---------------------------------------------------------------------------
// TradingStepContext: 子树构建所需的全部外部回调
// 由 TencentBot.TradingRoute.cpp 构造并传入，子树内部只通过回调访问外部能力。
// ---------------------------------------------------------------------------
struct TradingStepContext {
    // --- 运行控制 ---
    std::function<void()> check_stop;        // 检查停止信号（可能抛 StopRequestedException）
    std::function<void(int)> sleep_ms;       // 可中断 Sleep（内部轮询 stop 信号）

    // --- 地图感知 ---
    std::function<std::string()> get_current_map;  // 当前地图名

    // --- 断点状态（直接引用，非拷贝）---
    TradingCheckpoint* checkpoint = nullptr;
    CheckpointStore*   store      = nullptr;

    // --- 路线函数 ---
    struct {
        // Step0: 帮派→地府
        std::function<void()> leave_bangpai;
        std::function<void()> changan_to_datang;
        std::function<void()> datang_to_difu;

        // Step2: 地府→北俱（8 段）
        std::function<void()> leave_difu;
        std::function<void()> datang_to_chishui;
        std::function<void()> chishui_to_nvba;
        std::function<void()> nvba_to_donghaiyandong;
        std::function<void()> donghaiyandong_to_donghaiwan;
        std::function<void()> donghaiwan_to_aolai;
        std::function<void()> aolai_to_huaguo;
        std::function<void()> huaguo_to_beiju;

        // Step4: 北俱→地府
        std::function<void()> beiju_to_changan;
        std::function<void()> changan_to_datang_for_return;  // 复用 changan_to_datang
        std::function<void()> datang_to_difu_for_return;     // 复用 datang_to_difu

        // Goal: 回帮派
        std::function<void()> datang_to_changan_city;
        std::function<void()> changan_to_bangpai;
    } routes;

    // --- 商人导航 ---
    struct {
        std::function<void()> walk_to_difu_upper;
        std::function<void()> walk_to_difu_lower;
        std::function<void()> walk_to_beiju_upper;
        std::function<void()> walk_to_beiju_lower;
    } merchants;

    // --- 交易操作 ---
    struct {
        std::function<int(const std::string&, const std::string&)> query_buy_price;
        std::function<int(const std::string&, const std::string&)> query_sale_price;
        std::function<bool(const std::string&, const std::string&)> buy_item;
        std::function<int(const std::string&, const std::string&)> sell_item;   // 返回银票，-1=失败
    } trade;

    // --- 达标检测 ---
    // 卖出后调用，检查银票是否达标，若达标设置 checkpoint.is_goal_reached = true
    std::function<bool(const char* after_what, int money)> try_return_if_reached;

    // --- 验证码提交 ---
    std::function<void()> process_idiom_verify;

    // --- 屏幕截图重建 ---
    std::function<bool()> recreate_capture_if_needed;
};

// ---------------------------------------------------------------------------
// 子树构建函数
// 每个函数返回一棵完整的行为树子树，内部所有子步骤都是独立 BT 节点。
// ---------------------------------------------------------------------------

// [1/6] 出帮派 → 长安 → 大唐国境 → 地府
std::unique_ptr<bt::Node> BuildStep0_LeaveGangToDifu(
    TradingStepContext& ctx, bt::Observer obs);

// [2/6] 地府买纸钱（比价 + 购买 + 失败重试）
std::unique_ptr<bt::Node> BuildStep1_DifuBuyPaper(
    TradingStepContext& ctx, bt::Observer obs);

// [3/6] 地府 → … → 北俱泸州（8 段传送链）
std::unique_ptr<bt::Node> BuildStep2_TravelToBeiju(
    TradingStepContext& ctx, bt::Observer obs);

// [4/6] 北俱：卖纸钱（比价）+ 买油（+失败重试）
std::unique_ptr<bt::Node> BuildStep3_BeijuSellPaperBuyOil(
    TradingStepContext& ctx, bt::Observer obs);

// [5/6] 北俱 → 长安 → 大唐国境 → 地府
std::unique_ptr<bt::Node> BuildStep4_ReturnToDifu(
    TradingStepContext& ctx, bt::Observer obs);

// [6/6] 地府卖油（比价 + 卖出）
std::unique_ptr<bt::Node> BuildStep5_DifuSellOil(
    TradingStepContext& ctx, bt::Observer obs);

// 达标回帮派分支（作为 goal_branch 的子树）
std::unique_ptr<bt::Node> BuildGoalBranch(
    TradingStepContext& ctx, bt::Observer obs);

} // namespace domain
