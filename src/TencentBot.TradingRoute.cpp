#include "TencentBot.h"

#include "CheckpointStore.h"
#include "SharedDataStatus.h"
#include "config/BotSettings.h"
#include "domain/RunControl.h"
#include "domain/TradingRouteBehavior.h"
#include "domain/TradingStepTrees.h"

#include <windows.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

const config::TimingSettings& timing() {
    return config::GetBotSettings().timing;
}

const config::TradeThresholdSettings& threshold() {
    return config::GetBotSettings().trade_threshold;
}

// 运行期光标追踪线程：
// - 输出系统鼠标坐标
// - 同时输出 Host 回写的 SharedDataStatus 坐标，便于定位“鼠标乱飞/坐标错位”。
class CursorTraceRunner {
public:
    CursorTraceRunner(const std::atomic_bool* stop_signal, int interval_ms)
        : stop_signal_(stop_signal),
          interval_ms_(std::max(20, interval_ms)),
          worker_([this]() { run_loop(); }) {}

    ~CursorTraceRunner() {
        stop_.store(true, std::memory_order_relaxed);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void run_loop() {
        while (!stop_.load(std::memory_order_relaxed) &&
               !domain::is_stop_requested(stop_signal_)) {
            POINT pt{};
            if (GetCursorPos(&pt)) {
                const SharedDataSnapshot snap = read_shared_data_snapshot();
                BOT_LOG("CursorTrace",
                    "cursor=(" << pt.x << "," << pt.y << ")"
                    << " sync=" << snap.sync_flag
                    << " ts=" << snap.timestamp
                    << " pit=(" << snap.pit_x << "," << snap.pit_y << ")"
                    << " role=(" << snap.role_raw_x << "," << snap.role_raw_y << ")"
                    << " map=" << snap.map_id);
            } else {
                BOT_WARN("CursorTrace", "GetCursorPos failed");
            }

            int slept = 0;
            while (slept < interval_ms_ &&
                   !stop_.load(std::memory_order_relaxed) &&
                   !domain::is_stop_requested(stop_signal_)) {
                constexpr int kSliceMs = 20;
                const int chunk = std::min(kSliceMs, interval_ms_ - slept);
                std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
                slept += chunk;
            }
        }
    }

    const std::atomic_bool* stop_signal_ = nullptr;
    int interval_ms_ = 100;
    std::atomic_bool stop_{false};
    std::thread worker_;
};

} // namespace

void TencentBot::runTradingRoute() {
    BOT_LOG("TencentBot", "========== 跑商开始 ==========");

    const auto& runtime = config::GetBotSettings().runtime;
    [[maybe_unused]] std::unique_ptr<CursorTraceRunner> cursor_trace;
    if (runtime.log_cursor_during_run) {
        BOT_LOG("TencentBot",
            "运行期光标追踪已开启 interval_ms=" << runtime.cursor_interval_ms);
        cursor_trace = std::make_unique<CursorTraceRunner>(
            stopSignal_, static_cast<int>(runtime.cursor_interval_ms));
    }

    // --- 加载断点 ---
    CheckpointStore store(checkpointFile_);
    TradingCheckpoint ck;
    std::string ckError;
    if (store.load(ck, &ckError)) {
        BOT_LOG("TencentBot", "续跑: next_op=" << ck.next_op
                << " last=\"" << ck.last_op_name << "\" cycle=" << ck.cycle
                << " target=" << ck.target_money);
    } else {
        if (!ckError.empty()) BOT_WARN("TencentBot", "checkpoint 读取失败，从头开始: " << ckError);
        ck = TradingCheckpoint{};
        (void)store.save(ck);
    }

    if (screenCapture.recreateIfNeeded()) {
        BOT_LOG("TencentBot", "窗口捕获已恢复");
        domain::sleep_interruptible(stopSignal_, 500);
    }

    // --- Checkpoint 版本迁移 ---
    if (ck.version < 2) {
        ck.next_op += 1;
        ck.version = 2;
        ck.last_op_name = "migrated_from_v1";
        (void)store.save(ck);
        BOT_LOG("TencentBot", "checkpoint v1→v2, next_op=" << ck.next_op);
    }
    if (ck.version < 3) {
        ck.version = 3;
        ck.last_op_name = "migrated_to_v3";
        (void)store.save(ck);
        BOT_LOG("TencentBot", "checkpoint 已迁移到 v3");
    }

    // 步骤名称表（用于 checkpoint 日志）
    const std::vector<std::string> stepNames = {
        "leave_gang_to_difu",
        "difu_buy_paper",
        "travel_to_beiju",
        "beiju_sell_paper_buy_oil",
        "return_to_difu",
        "difu_sell_oil",
    };
    const int stepCount = static_cast<int>(stepNames.size());

    if (ck.next_op < 0) ck.next_op = 0;
    if (ck.next_op > stepCount) ck.next_op = stepCount;

    // 后续循环跳过 step0（出帮派），从 step1 开始
    constexpr int kCycleRestartFromStep = 1;

    // --- 达标检测回调 ---
    auto tryReturnToGangIfReached = [&](const char* afterWhat, int money) -> bool {
        domain::check_stop_or_throw(stopSignal_);
        BOT_LOG("TencentBot", "(" << afterWhat << ") 银票=" << money << " 目标=" << ck.target_money);
        if (!(money >= 0 && money >= ck.target_money)) return false;

        BOT_LOG("TencentBot", "已达标，记录断点并跳转到回帮步骤");
        ck.is_goal_reached = true;
        ck.last_op_name = std::string("goal_reached_after_") + afterWhat;
        (void)store.save(ck);
        return true;
    };

    // --- 构建 TradingStepContext（注入所有外部回调）---
    domain::TradingStepContext stepCtx{};
    stepCtx.check_stop = [&]() { domain::check_stop_or_throw(stopSignal_); };
    stepCtx.sleep_ms = [&](int ms) { domain::sleep_interruptible(stopSignal_, ms); };
    stepCtx.get_current_map = [&]() -> std::string { return vision.getCurrentMapName(); };
    stepCtx.checkpoint = &ck;
    stepCtx.store = &store;

    // 路线函数绑定
    stepCtx.routes.leave_bangpai         = [&]() { route_leaveBangpai(); };
    stepCtx.routes.changan_to_datang     = [&]() { route_changan_to_datangguojing(); };
    stepCtx.routes.datang_to_difu        = [&]() { route_datangguojing_to_difu(); };
    stepCtx.routes.leave_difu            = [&]() { route_leaveDisfu(); };
    stepCtx.routes.datang_to_chishui     = [&]() { route_datangguojing_to_chishuizhou(); };
    stepCtx.routes.chishui_to_nvba       = [&]() { route_chishuizhou_to_nvbamu(); };
    stepCtx.routes.nvba_to_donghaiyandong      = [&]() { route_nvbamu_to_donghaiyandong(); };
    stepCtx.routes.donghaiyandong_to_donghaiwan = [&]() { route_donghaiyandong_to_donghaiwan(); };
    stepCtx.routes.donghaiwan_to_aolai   = [&]() { route_donghaiwan_to_aolaiguo(); };
    stepCtx.routes.aolai_to_huaguo       = [&]() { route_aolaiguo_to_huaguoshan(); };
    stepCtx.routes.huaguo_to_beiju       = [&]() { route_huaguoshan_to_beijuluzhou(); };
    stepCtx.routes.beiju_to_changan      = [&]() { route_beijuluzhou_to_changan(); };
    stepCtx.routes.changan_to_datang_for_return  = [&]() { route_changan_to_datangguojing(); };
    stepCtx.routes.datang_to_difu_for_return     = [&]() { route_datangguojing_to_difu(); };
    stepCtx.routes.datang_to_changan_city = [&]() { route_datangguojing_to_changancheng(); };
    stepCtx.routes.changan_to_bangpai    = [&]() { route_changan_to_bangpai(); };

    // 商人导航
    stepCtx.merchants.walk_to_difu_upper  = [&]() { walkToDifuUpperMerchant(); };
    stepCtx.merchants.walk_to_difu_lower  = [&]() { walkToDifuLowerMerchant(); };
    stepCtx.merchants.walk_to_beiju_upper = [&]() { walkToBeixuUpperMerchant(); };
    stepCtx.merchants.walk_to_beiju_lower = [&]() { walkToBeixuLowerMerchant(); };

    // 交易操作
    stepCtx.trade.query_buy_price  = [&](const std::string& npc, const std::string& item) { return queryNpcBuyPrice(npc, item); };
    stepCtx.trade.query_sale_price = [&](const std::string& npc, const std::string& item) { return queryNpcSalePrice(npc, item); };
    stepCtx.trade.buy_item         = [&](const std::string& npc, const std::string& item) { return buyItemFromNpc(npc, item); };
    stepCtx.trade.sell_item        = [&](const std::string& npc, const std::string& item) { return sellItemToNpc(npc, item); };

    // 达标检测
    stepCtx.try_return_if_reached = tryReturnToGangIfReached;

    // 验证码
    stepCtx.process_idiom_verify = [&]() { process_idiom_verify(); };

    // 截图重建
    stepCtx.recreate_capture_if_needed = [&]() { return screenCapture.recreateIfNeeded(); };

    // --- 行为树观测器 ---
    std::map<std::string, domain::TreeStatus> btLastStatus;
    auto btObserver = [&](std::string_view node, domain::TreeStatus status) {
        const std::string key(node);
        auto it = btLastStatus.find(key);
        if (it == btLastStatus.end() || it->second != status) {
            btLastStatus[key] = status;
            BOT_LOG("BehaviorTree", "node=" << key << " status=" << domain::status_to_cstr(status));
        }
    };

    // Observer 转换为 bt::Observer（用于子树构建）
    bt::Observer btObs = [&btObserver](std::string_view node, bt::Status status) {
        domain::TreeStatus ts;
        switch (status) {
            case bt::Status::Success: ts = domain::TreeStatus::Success; break;
            case bt::Status::Failure: ts = domain::TreeStatus::Failure; break;
            case bt::Status::Running: ts = domain::TreeStatus::Running; break;
            default: ts = domain::TreeStatus::Failure; break;
        }
        btObserver(node, ts);
    };

    // --- 构建 6 棵步骤子树 ---
    std::vector<domain::StepSubtreeNode> subtreeNodes;
    subtreeNodes.reserve(stepCount);

    // 每棵子树用 wrapper Action 包装：执行前打日志、执行后推进 checkpoint
    auto wrapStep = [&](int i, std::unique_ptr<bt::Node> subtree) -> std::unique_ptr<bt::Node> {
        // 在原始子树外面套一层 Action：
        //   - 执行子树 tick
        //   - Success 时推进 checkpoint
        //   - Running/Failure 透传
        // 注：这里用 shared_ptr 持有子树，因为 lambda 需要可拷贝（std::function 要求）
        auto sharedSubtree = std::shared_ptr<bt::Node>(std::move(subtree));
        return std::make_unique<bt::Action>(
            "wrapped_" + stepNames[i],
            [&, i, sharedSubtree]() -> bt::Status {
                domain::check_stop_or_throw(stopSignal_);
                BOT_LOG("TencentBot", "=== step " << i << "/" << stepCount
                        << " : " << stepNames[i] << " (cycle=" << ck.cycle << ") ===");
                const bt::Status stepStatus = sharedSubtree->tick();
                if (stepStatus != bt::Status::Success) {
                    return stepStatus;
                }
                if (!ck.is_goal_reached) {
                    ck.next_op = i + 1;
                    ck.last_op_name = stepNames[i];
                    (void)store.save(ck);
                }
                return bt::Status::Success;
            },
            btObs
        );
    };

    subtreeNodes.push_back(domain::StepSubtreeNode{
        "leave_gang_to_difu",
        wrapStep(0, domain::BuildStep0_LeaveGangToDifu(stepCtx, btObs))
    });
    subtreeNodes.push_back(domain::StepSubtreeNode{
        "difu_buy_paper",
        wrapStep(1, domain::BuildStep1_DifuBuyPaper(stepCtx, btObs))
    });
    subtreeNodes.push_back(domain::StepSubtreeNode{
        "travel_to_beiju",
        wrapStep(2, domain::BuildStep2_TravelToBeiju(stepCtx, btObs))
    });
    subtreeNodes.push_back(domain::StepSubtreeNode{
        "beiju_sell_paper_buy_oil",
        wrapStep(3, domain::BuildStep3_BeijuSellPaperBuyOil(stepCtx, btObs))
    });
    subtreeNodes.push_back(domain::StepSubtreeNode{
        "return_to_difu",
        wrapStep(4, domain::BuildStep4_ReturnToDifu(stepCtx, btObs))
    });
    subtreeNodes.push_back(domain::StepSubtreeNode{
        "difu_sell_oil",
        wrapStep(5, domain::BuildStep5_DifuSellOil(stepCtx, btObs))
    });

    // --- 轮次重启回调 ---
    auto btCycleRestartIfNeeded = [&]() -> domain::TreeStatus {
        if (ck.next_op < 0) ck.next_op = 0;
        if (ck.next_op > stepCount) ck.next_op = stepCount;
        if (ck.next_op < stepCount) {
            return domain::TreeStatus::Failure;
        }

        ck.cycle += 1;
        ck.next_op = kCycleRestartFromStep;
        ck.last_op_name = "cycle_restart";
        (void)store.save(ck);
        BOT_LOG("TencentBot", "未达标，继续下一轮 cycle=" << ck.cycle);
        return domain::TreeStatus::Success;
    };

    // --- 组装行为树构建上下文（子树模式）---
    domain::TradingTreeBuildContext treeCtx{};
    treeCtx.goal_subtree = domain::BuildGoalBranch(stepCtx, btObs);
    treeCtx.step_subtrees = std::move(subtreeNodes);
    treeCtx.current_step_index = [&]() { return ck.next_op; };
    treeCtx.cycle_restart_if_needed = btCycleRestartIfNeeded;
    treeCtx.observer = btObserver;

    domain::TradingRouteBehavior behavior(std::move(treeCtx));

    try {
        domain::RunTradingRouteTreeLoop(
            behavior,
            [&]() {
                domain::check_stop_or_throw(stopSignal_);
            },
            [&]() {
                BOT_WARN("TencentBot", "行为树 tick 失败，1 秒后重试");
                domain::sleep_interruptible(stopSignal_, 1000);
            }
        );
    } catch (const domain::StopRequestedException&) {
        (void)store.save(ck);
        BOT_LOG("TencentBot", "已请求停止，checkpoint 已保存 (next_op=" << ck.next_op << ")");
    } catch (const domain::GoalReachedException&) {
        BOT_LOG("TencentBot", "========== 跑商完成 ==========");
    }
}
