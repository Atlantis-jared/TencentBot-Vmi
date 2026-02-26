#include "TencentBot.h"

#include "CheckpointStore.h"
#include "config/BotSettings.h"
#include "domain/RunControl.h"
#include "domain/TradingRouteBehavior.h"

#include <windows.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

const config::TimingSettings& timing() {
    return config::GetBotSettings().timing;
}

const config::TradeThresholdSettings& threshold() {
    return config::GetBotSettings().trade_threshold;
}

} // namespace

void TencentBot::runTradingRoute() {
    BOT_LOG("TencentBot", "========== 跑商开始 ==========");

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
        Sleep(500);
    }

    // --- 跟踪实际买入价（用于卖出时计算利润率）---
    int lastPaperBuyPrice = 0;  // 上次买纸钱的实际价格
    int lastOilBuyPrice   = 0;  // 上次买油的实际价格

    // --- 银票检测 & 达标回帮派 ---
    auto readMoneyFromPanel = [&](const std::string& npcName) -> int {
        auto ensureNearNpc = [&]() {
            if (npcName == "地府商人")      walkToDifuLowerMerchant();
            else if (npcName == "地府货商") walkToDifuUpperMerchant();
            else if (npcName == "北俱商人") walkToBeixuLowerMerchant();
            else if (npcName == "北俱货商") walkToBeixuUpperMerchant();
        };
        for (int trial = 0; trial < 2; ++trial) {
            domain::check_stop_or_throw(stopSignal_);
            if (trial > 0) { ensureNearNpc(); Sleep(timing().ui_update_delay_ms); }
            if (!clickNpcIfFound(npcName, 0, -40)) continue;
            clickUiElement(242, 473);
            moveCharacterTo(200, 70, 0);
            Sleep(timing().ui_update_delay_ms);
            int money = readCurrentMoney();
            auto cancelHits = vision.findNpcOnScreen("取消");
            if (!cancelHits.empty()) clickUiElement(cancelHits[0].x, cancelHits[0].y);
            if (money >= 0) return money;
        }
        return -10;
    };

    auto step6_returnAndSubmit = [&]() -> domain::TreeStatus {
        domain::check_stop_or_throw(stopSignal_);
        BOT_LOG("TencentBot", "[6/6] 达标回帮派 + 提交银票");

        const std::string curMap = vision.getCurrentMapName();
        if (curMap == "beijuluzhou") {
            route_beijuluzhou_to_changan();
            return domain::TreeStatus::Running;
        }
        if (curMap == "difu") {
            route_leaveDisfu();
            return domain::TreeStatus::Running;
        }
        if (curMap == "datangguojing") {
            route_datangguojing_to_changancheng();
            return domain::TreeStatus::Running;
        }
        if (curMap == "changancheng") {
            route_changan_to_bangpai();
            return domain::TreeStatus::Running;
        }

        if (curMap == "bangpai" || curMap == "jinku") {
            process_idiom_verify(); // 提交银票并处理可能出现的成语验证码
            BOT_LOG("TencentBot", "已返回帮派并提交银票，跑商完成");
            (void)store.clear();
            domain::throw_goal_reached();
        }

        BOT_ERR("TencentBot", "当前地图不在回城路径上(" << curMap << ")，请手动回城完成最后提交");
        ck.last_op_name = "return_path_unexpected_map";
        (void)store.save(ck);
        return domain::TreeStatus::Failure;
    };

    auto tryReturnToGangIfReached = [&](const char* afterWhat, int money) -> bool {
        domain::check_stop_or_throw(stopSignal_);
        BOT_LOG("TencentBot", "(" << afterWhat << ") 银票=" << money << " 目标=" << ck.target_money);
        if (!(money >= 0 && money >= ck.target_money)) return false;

        BOT_LOG("TencentBot", "已达标，记录断点并跳转到回帮步骤");
        ck.is_goal_reached = true;
        ck.last_op_name = std::string("goal_reached_after_") + afterWhat;
        (void)store.save(ck); // 保存关键的 is_goal_reached 状态
        return true;
    };

    // --- 6 个操作步骤 ---
    struct Step0State {
        int phase = 0;
    };
    auto step0State = std::make_shared<Step0State>();
    auto step0_leaveBangpaiToDifu = [&, step0State]() -> domain::TreeStatus {
        domain::check_stop_or_throw(stopSignal_);
        if (step0State->phase == 0) {
            BOT_LOG("TencentBot", "[1/6] 出帮派 → 长安 → 大唐国境 → 地府");
            route_leaveBangpai();
            step0State->phase = 1;
            return domain::TreeStatus::Running;
        }
        if (step0State->phase == 1) {
            route_changan_to_datangguojing();
            step0State->phase = 2;
            return domain::TreeStatus::Running;
        }
        route_datangguojing_to_difu();
        step0State->phase = 0;
        return domain::TreeStatus::Success;
    };

    struct Step1State {
        int phase = 0;
        int price1 = -1;
        int price2 = -1;
        std::string bestMerchant;
        std::string otherMerchant;
        std::chrono::steady_clock::time_point retryAt{};
    };
    auto step1State = std::make_shared<Step1State>();
    auto step1_difuBuyPaper = [&, step1State]() -> domain::TreeStatus {
        domain::check_stop_or_throw(stopSignal_);
        if (step1State->phase == 0) {
            BOT_LOG("TencentBot", "[2/6] 地府买纸钱");
            step1State->price1 = -1;
            step1State->price2 = -1;
            step1State->bestMerchant = ck.preferred_difu_merchant;

            if (!step1State->bestMerchant.empty()) {
                BOT_LOG("TencentBot", "  使用首选地府商人: " << step1State->bestMerchant);
            } else {
                // 首次运行，需要比价
                walkToDifuUpperMerchant();
                step1State->price1 = queryNpcBuyPrice("地府货商", "纸钱");
                if (step1State->price1 < 0) {
                    Sleep(timing().ui_update_delay_ms);
                    walkToDifuUpperMerchant();
                    step1State->price1 = queryNpcBuyPrice("地府货商", "纸钱");
                }
                BOT_LOG("TencentBot", "  地府货商 纸钱买价="
                        << (step1State->price1 > 0 ? std::to_string(step1State->price1) : "失败"));

                if (step1State->price1 > 0 && step1State->price1 <= threshold().paper_buy_price_max) {
                    step1State->bestMerchant = "地府货商";
                } else {
                    walkToDifuLowerMerchant();
                    step1State->price2 = queryNpcBuyPrice("地府商人", "纸钱");
                    if (step1State->price2 < 0) {
                        Sleep(timing().ui_update_delay_ms);
                        walkToDifuLowerMerchant();
                        step1State->price2 = queryNpcBuyPrice("地府商人", "纸钱");
                    }
                    BOT_LOG("TencentBot", "  地府商人 纸钱买价="
                            << (step1State->price2 > 0 ? std::to_string(step1State->price2) : "失败"));

                    if (step1State->price1 > 0 &&
                        (step1State->price2 < 0 || step1State->price1 <= step1State->price2)) {
                        step1State->bestMerchant = "地府货商";
                    } else {
                        step1State->bestMerchant = "地府商人";
                    }
                }
                ck.preferred_difu_merchant = step1State->bestMerchant;
                (void)store.save(ck);
            }

            step1State->otherMerchant =
                (step1State->bestMerchant == "地府货商") ? "地府商人" : "地府货商";
            step1State->phase = 1;
            return domain::TreeStatus::Running;
        }

        if (step1State->phase == 1) {
            if (step1State->bestMerchant == "地府货商") walkToDifuUpperMerchant(); else walkToDifuLowerMerchant();
            if (buyItemFromNpc(step1State->bestMerchant, "纸钱")) {
                lastPaperBuyPrice = (step1State->price1 > 0) ? step1State->price1 :
                                    ((step1State->price2 > 0) ? step1State->price2 : 2700);
                BOT_LOG("TencentBot", "  购买成功，价格(估)=" << lastPaperBuyPrice);
                step1State->phase = 0;
                return domain::TreeStatus::Success;
            }
            BOT_WARN("TencentBot", "首选商人 " << step1State->bestMerchant
                    << " 购买失败，尝试备选 " << step1State->otherMerchant);
            step1State->phase = 2;
            return domain::TreeStatus::Running;
        }

        if (step1State->phase == 2) {
            if (step1State->otherMerchant == "地府货商") walkToDifuUpperMerchant(); else walkToDifuLowerMerchant();
            if (buyItemFromNpc(step1State->otherMerchant, "纸钱")) {
                lastPaperBuyPrice = (step1State->price2 > 0) ? step1State->price2 :
                                    ((step1State->price1 > 0) ? step1State->price1 : 2700);
                BOT_LOG("TencentBot", "  备选购买成功，价格(估)=" << lastPaperBuyPrice);
                ck.preferred_difu_merchant = step1State->otherMerchant;
                (void)store.save(ck);
                step1State->phase = 0;
                return domain::TreeStatus::Success;
            }
            BOT_WARN("TencentBot", "地府纸钱可能全卖完了，30 秒后重试...");
            step1State->retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            step1State->phase = 3;
            return domain::TreeStatus::Running;
        }

        if (std::chrono::steady_clock::now() < step1State->retryAt) {
            return domain::TreeStatus::Running;
        }
        step1State->phase = 0;
        return domain::TreeStatus::Running;
    };

    struct Step2State {
        int phase = 0;
    };
    auto step2State = std::make_shared<Step2State>();
    auto step2_travelToBeiju = [&, step2State]() -> domain::TreeStatus {
        domain::check_stop_or_throw(stopSignal_);
        if (step2State->phase == 0) {
            BOT_LOG("TencentBot", "[3/6] 地府 → … → 北俱泸州（8段传送）");
        }
        switch (step2State->phase) {
            case 0: route_leaveDisfu(); step2State->phase = 1; return domain::TreeStatus::Running;
            case 1: route_datangguojing_to_chishuizhou(); step2State->phase = 2; return domain::TreeStatus::Running;
            case 2: route_chishuizhou_to_nvbamu(); step2State->phase = 3; return domain::TreeStatus::Running;
            case 3: route_nvbamu_to_donghaiyandong(); step2State->phase = 4; return domain::TreeStatus::Running;
            case 4: route_donghaiyandong_to_donghaiwan(); step2State->phase = 5; return domain::TreeStatus::Running;
            case 5: route_donghaiwan_to_aolaiguo(); step2State->phase = 6; return domain::TreeStatus::Running;
            case 6: route_aolaiguo_to_huaguoshan(); step2State->phase = 7; return domain::TreeStatus::Running;
            case 7:
                route_huaguoshan_to_beijuluzhou();
                step2State->phase = 0;
                return domain::TreeStatus::Success;
            default:
                step2State->phase = 0;
                return domain::TreeStatus::Failure;
        }
    };

    struct Step3State {
        int phase = 0;
        int salePrice1 = -1;
        int salePrice2 = -1;
        std::string sellTarget;
        std::string buyTarget;
        std::chrono::steady_clock::time_point retryAt{};
    };
    auto step3State = std::make_shared<Step3State>();
    auto step3_beixuSellPaperBuyOil = [&, step3State]() -> domain::TreeStatus {
        domain::check_stop_or_throw(stopSignal_);
        if (step3State->phase == 0) {
            BOT_LOG("TencentBot", "[4/6] 北俱: 卖纸钱 + 买油");
            walkToBeixuUpperMerchant();
            step3State->salePrice1 = queryNpcSalePrice("北俱货商", "纸钱");
            if (step3State->salePrice1 < 0) {
                Sleep(timing().ui_update_delay_ms);
                walkToBeixuUpperMerchant();
                step3State->salePrice1 = queryNpcSalePrice("北俱货商", "纸钱");
            }
            BOT_LOG("TencentBot", "  北俱货商 纸钱收购价=" << step3State->salePrice1);

            walkToBeixuLowerMerchant();
            step3State->salePrice2 = queryNpcSalePrice("北俱商人", "纸钱");
            if (step3State->salePrice2 < 0) {
                Sleep(timing().ui_update_delay_ms);
                walkToBeixuLowerMerchant();
                step3State->salePrice2 = queryNpcSalePrice("北俱商人", "纸钱");
            }
            BOT_LOG("TencentBot", "  北俱商人 纸钱收购价=" << step3State->salePrice2);

            if (step3State->salePrice1 >= step3State->salePrice2) {
                step3State->sellTarget = "北俱货商";
                step3State->buyTarget = "北俱商人";
            } else {
                step3State->sellTarget = "北俱商人";
                step3State->buyTarget = "北俱货商";
            }
            BOT_LOG("TencentBot", "  决策: 卖给 " << step3State->sellTarget
                    << "，从 " << step3State->buyTarget << " 买油");
            step3State->phase = 1;
            return domain::TreeStatus::Running;
        }

        if (step3State->phase == 1) {
            if (step3State->sellTarget == "北俱货商") walkToBeixuUpperMerchant();
            else walkToBeixuLowerMerchant();

            const int paperMoney = sellItemToNpc(step3State->sellTarget, "纸钱");
            (void)tryReturnToGangIfReached("beiju_sale_paper", paperMoney);
            step3State->phase = 2;
            return domain::TreeStatus::Running;
        }

        if (step3State->phase == 2) {
            if (step3State->buyTarget == "北俱货商") walkToBeixuUpperMerchant();
            else walkToBeixuLowerMerchant();
            BOT_LOG("TencentBot", "  尝试向 " << step3State->buyTarget << " 买油");
            if (buyItemFromNpc(step3State->buyTarget, "油")) {
                lastOilBuyPrice = 3500;
                step3State->phase = 0;
                return domain::TreeStatus::Success;
            }
            step3State->phase = 3;
            return domain::TreeStatus::Running;
        }

        if (step3State->phase == 3) {
            const std::string altTarget =
                (step3State->buyTarget == "北俱货商") ? "北俱商人" : "北俱货商";
            BOT_WARN("TencentBot", "首选买油商人 " << step3State->buyTarget
                    << " 缺货，尝试备选 " << altTarget);
            if (altTarget == "北俱货商") walkToBeixuUpperMerchant(); else walkToBeixuLowerMerchant();
            if (buyItemFromNpc(altTarget, "油")) {
                lastOilBuyPrice = 3500;
                step3State->phase = 0;
                return domain::TreeStatus::Success;
            }
            BOT_WARN("TencentBot", "北俱油可能全卖完了，30 秒后重试...");
            step3State->retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            step3State->phase = 4;
            return domain::TreeStatus::Running;
        }

        if (std::chrono::steady_clock::now() < step3State->retryAt) {
            return domain::TreeStatus::Running;
        }
        step3State->phase = 2;
        return domain::TreeStatus::Running;
    };

    struct Step4State {
        int phase = 0;
    };
    auto step4State = std::make_shared<Step4State>();
    auto step4_returnToDifu = [&, step4State]() -> domain::TreeStatus {
        domain::check_stop_or_throw(stopSignal_);
        if (step4State->phase == 0) {
            BOT_LOG("TencentBot", "[5/6] 北俱 → 长安 → 大唐国境 → 地府");
            route_beijuluzhou_to_changan();
            step4State->phase = 1;
            return domain::TreeStatus::Running;
        }
        if (step4State->phase == 1) {
            route_changan_to_datangguojing();
            step4State->phase = 2;
            return domain::TreeStatus::Running;
        }
        route_datangguojing_to_difu();
        step4State->phase = 0;
        return domain::TreeStatus::Success;
    };

    struct Step5State {
        int phase = 0;
        int oilSalePrice1 = -1;
        int oilSalePrice2 = -1;
        std::string sellTarget;
        std::string nextBuyTarget;
    };
    auto step5State = std::make_shared<Step5State>();
    auto step5_difuSellOil = [&, step5State]() -> domain::TreeStatus {
        domain::check_stop_or_throw(stopSignal_);
        if (step5State->phase == 0) {
            BOT_LOG("TencentBot", "[6/6] 地府卖油");
            walkToDifuUpperMerchant();
            step5State->oilSalePrice1 = queryNpcSalePrice("地府货商", "油");
            if (step5State->oilSalePrice1 < 0) {
                Sleep(timing().ui_update_delay_ms);
                walkToDifuUpperMerchant();
                step5State->oilSalePrice1 = queryNpcSalePrice("地府货商", "油");
            }
            BOT_LOG("TencentBot", "  地府货商 油收购价=" << step5State->oilSalePrice1);
            step5State->phase = 1;
            return domain::TreeStatus::Running;
        }

        if (step5State->phase == 1) {
            walkToDifuLowerMerchant();
            step5State->oilSalePrice2 = queryNpcSalePrice("地府商人", "油");
            if (step5State->oilSalePrice2 < 0) {
                Sleep(timing().ui_update_delay_ms);
                walkToDifuLowerMerchant();
                step5State->oilSalePrice2 = queryNpcSalePrice("地府商人", "油");
            }
            BOT_LOG("TencentBot", "  地府商人 油收购价=" << step5State->oilSalePrice2);

            if (step5State->oilSalePrice1 >= step5State->oilSalePrice2) {
                step5State->sellTarget = "地府货商";
                step5State->nextBuyTarget = "地府商人";
            } else {
                step5State->sellTarget = "地府商人";
                step5State->nextBuyTarget = "地府货商";
            }
            BOT_LOG("TencentBot", "  决策: 卖给 " << step5State->sellTarget
                    << "，下一轮首选买家更新为 " << step5State->nextBuyTarget);
            step5State->phase = 2;
            return domain::TreeStatus::Running;
        }

        if (step5State->sellTarget == "地府货商") walkToDifuUpperMerchant();
        else walkToDifuLowerMerchant();

        const int money = sellItemToNpc(step5State->sellTarget, "油");
        (void)tryReturnToGangIfReached("difu_sale_oil", money);
        ck.preferred_difu_merchant = step5State->nextBuyTarget;
        (void)store.save(ck);
        step5State->phase = 0;
        return domain::TreeStatus::Success;
    };

    // --- 操作表（名称 + 函数）---
    struct TradeStep {
        const char* name;
        std::function<domain::TreeStatus()> execute;
    };
    std::vector<TradeStep> steps = {
        {"leave_gang_to_difu",       step0_leaveBangpaiToDifu},
        {"difu_buy_paper",           step1_difuBuyPaper},
        {"travel_to_beiju",          step2_travelToBeiju},
        {"beiju_sell_paper_buy_oil", step3_beixuSellPaperBuyOil},
        {"return_to_difu",           step4_returnToDifu},
        {"difu_sell_oil",            step5_difuSellOil},
        // {"return_and_submit",        step6_returnAndSubmit}, // 不再顺序执行，仅由 tryReturnToGangIfReached 触发
    };

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

    if (ck.next_op < 0) ck.next_op = 0;
    if (ck.next_op > static_cast<int>(steps.size())) ck.next_op = static_cast<int>(steps.size());

    // 后续循环跳过 step0（出帮派），从 step1 开始
    constexpr int kCycleRestartFromStep = 1;

    // --- 行为树版本主循环 ---
    // 节点设计（显式分发）：
    // 1) goal_branch: 目标达成后执行“回帮派并提交”
    // 2) step_dispatch_branch:
    //    - 每个 step 都是一个 Sequence(Condition(next_op==i), Action(run_step_i))
    //    - 所有 step Sequence 放入 Selector，实现“按 next_op 选择执行节点”
    // 3) cycle_restart_if_needed:
    //    - 当 next_op == steps.size() 时触发新一轮 cycle
    //
    // 根节点 route_root 仍使用 Selector，优先尝试 goal_branch，失败后走 step_dispatch_branch。
    auto btGoalCondition = [&]() -> bool {
        return ck.is_goal_reached;
    };

    // 当银票达标后，该动作负责执行最终提交流程。
    // step6_returnAndSubmit 内部在成功时会抛 GoalReachedException 结束主循环。
    auto btGoalAction = [&]() -> domain::TreeStatus {
        return step6_returnAndSubmit();
    };

    // 封装单步执行（供每个 step Action 节点复用）。
    auto btRunStep = [&](int i) -> domain::TreeStatus {
        if (i < 0 || i >= static_cast<int>(steps.size())) {
            return domain::TreeStatus::Failure;
        }
        domain::check_stop_or_throw(stopSignal_);
        BOT_LOG("TencentBot", "=== step " << i << "/" << steps.size()
                << " : " << steps[i].name << " (cycle=" << ck.cycle << ") ===");
        const domain::TreeStatus stepStatus = steps[i].execute();
        if (stepStatus != domain::TreeStatus::Success) {
            return stepStatus;
        }

        if (!ck.is_goal_reached) {
            // 仅在未达标时推进断点；达标分支会由 goal_branch 接管。
            ck.next_op = i + 1;
            ck.last_op_name = steps[i].name;
            (void)store.save(ck);
        }
        return domain::TreeStatus::Success;
    };

    // 封装“轮次边界处理”：
    // 只有当 next_op 已走到末尾时才返回 Success，否则返回 Failure 让 selector 继续其它分支。
    auto btCycleRestartIfNeeded = [&]() -> domain::TreeStatus {
        if (ck.next_op < 0) ck.next_op = 0;
        if (ck.next_op > static_cast<int>(steps.size())) ck.next_op = static_cast<int>(steps.size());
        if (ck.next_op < static_cast<int>(steps.size())) {
            return domain::TreeStatus::Failure;
        }

        ck.cycle += 1;
        ck.next_op = kCycleRestartFromStep;
        ck.last_op_name = "cycle_restart";
        (void)store.save(ck);
        BOT_LOG("TencentBot", "未达标，继续下一轮 cycle=" << ck.cycle);
        return domain::TreeStatus::Success;
    };

    // 行为树节点状态观测：
    // 仅在状态变化时打日志，避免每 tick 刷屏。
    std::map<std::string, domain::TreeStatus> btLastStatus;
    auto btObserver = [&](std::string_view node, domain::TreeStatus status) {
        const std::string key(node);
        auto it = btLastStatus.find(key);
        if (it == btLastStatus.end() || it->second != status) {
            btLastStatus[key] = status;
            BOT_LOG("BehaviorTree", "node=" << key << " status=" << domain::status_to_cstr(status));
        }
    };

    // 将步骤执行器映射为 domain 层可消费的抽象节点定义。
    std::vector<domain::TradingStepNode> stepNodes;
    stepNodes.reserve(steps.size());
    for (int i = 0; i < static_cast<int>(steps.size()); ++i) {
        stepNodes.push_back(domain::TradingStepNode{
            std::string(steps[i].name),
            [&, i]() { return btRunStep(i); }
        });
    }

    domain::TradingTreeBuildContext treeCtx{};
    treeCtx.is_goal_reached = btGoalCondition;
    treeCtx.run_goal_action = btGoalAction;
    treeCtx.steps = std::move(stepNodes);
    treeCtx.current_step_index = [&]() { return ck.next_op; };
    treeCtx.cycle_restart_if_needed = btCycleRestartIfNeeded;
    treeCtx.observer = btObserver;

    domain::TradingRouteBehavior behavior(std::move(treeCtx));

    try {
        domain::RunTradingRouteTreeLoop(
            behavior,
            [&]() {
                // 非阻塞行为树模式下，step 动作在后台执行；
                // 这里仅做停止检查，避免与后台动作并发读写 checkpoint 状态。
                domain::check_stop_or_throw(stopSignal_);
            },
            [&]() {
                BOT_WARN("TencentBot", "行为树 tick 失败，1 秒后重试");
                Sleep(1000);
            }
        );
    } catch (const domain::StopRequestedException&) {
        // 收到停止信号时，保证 checkpoint 至少落盘一次。
        (void)store.save(ck);
        BOT_LOG("TencentBot", "已请求停止，checkpoint 已保存 (next_op=" << ck.next_op << ")");
    } catch (const domain::GoalReachedException&) {
        // 目标达成是正常业务收尾，不视为异常错误。
        BOT_LOG("TencentBot", "========== 跑商完成 ==========");
    }
}

