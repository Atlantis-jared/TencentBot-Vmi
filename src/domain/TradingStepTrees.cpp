#include "TradingStepTrees.h"

#include "../bt/BehaviorTree.h"
#include "../bt/RetryDecorator.h"
#include "../bt/WaitAction.h"
#include "RunControl.h"
#include "../BotLogger.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace domain {

namespace {

const config::TimingSettings& timing() {
    return config::GetBotSettings().timing;
}

const config::TradeThresholdSettings& threshold() {
    return config::GetBotSettings().trade_threshold;
}

// 把 void() 回调包装成 Action 节点（执行后返回 Success）。
std::unique_ptr<bt::Node> MakeVoidAction(
    std::string name,
    std::function<void()> fn,
    std::function<void()> check_stop,
    bt::Observer obs
) {
    return std::make_unique<bt::Action>(
        std::move(name),
        [fn = std::move(fn), check_stop = std::move(check_stop)]() -> bt::Status {
            if (check_stop) check_stop();
            if (fn) fn();
            return bt::Status::Success;
        },
        std::move(obs)
    );
}

} // namespace

// =============================================================================
// Step0: 出帮派 → 长安 → 大唐国境 → 地府
// 原来的 3-phase 状态机，拆为 Sequence(3 个 Action)。
// =============================================================================
std::unique_ptr<bt::Node> BuildStep0_LeaveGangToDifu(
    TradingStepContext& ctx, bt::Observer obs
) {
    std::vector<std::unique_ptr<bt::Node>> children;
    children.reserve(4);

    children.emplace_back(std::make_unique<bt::Action>(
        "step0_log",
        [&ctx]() -> bt::Status {
            if (ctx.check_stop) ctx.check_stop();
            BOT_LOG("TencentBot", "[1/6] 出帮派 → 长安 → 大唐国境 → 地府");
            return bt::Status::Success;
        },
        obs
    ));

    children.emplace_back(MakeVoidAction(
        "route_leave_bangpai", ctx.routes.leave_bangpai, ctx.check_stop, obs));

    children.emplace_back(MakeVoidAction(
        "route_changan_to_datang", ctx.routes.changan_to_datang, ctx.check_stop, obs));

    children.emplace_back(MakeVoidAction(
        "route_datang_to_difu", ctx.routes.datang_to_difu, ctx.check_stop, obs));

    return std::make_unique<bt::Sequence>("leave_gang_to_difu", std::move(children), obs);
}

// =============================================================================
// Step1: 地府买纸钱
// 原来的 4-phase + 重试计时器，拆为：
//   Sequence(compare_prices → Selector(buy_best → buy_other → retry_wait))
// =============================================================================
std::unique_ptr<bt::Node> BuildStep1_DifuBuyPaper(
    TradingStepContext& ctx, bt::Observer obs
) {
    // 共享状态（替代原来的 Step1State）
    struct State {
        int price1 = -1;
        int price2 = -1;
        std::string bestMerchant;
        std::string otherMerchant;
        int lastPaperBuyPrice = 2700;
    };
    auto state = std::make_shared<State>();

    std::vector<std::unique_ptr<bt::Node>> seq_children;

    // --- 子节点 1: 日志 ---
    seq_children.emplace_back(std::make_unique<bt::Action>(
        "step1_log",
        [&ctx]() -> bt::Status {
            if (ctx.check_stop) ctx.check_stop();
            BOT_LOG("TencentBot", "[2/6] 地府买纸钱");
            return bt::Status::Success;
        },
        obs
    ));

    // --- 子节点 2: 比价确定最优商人 ---
    seq_children.emplace_back(std::make_unique<bt::Action>(
        "difu_compare_paper_prices",
        [&ctx, state]() -> bt::Status {
            if (ctx.check_stop) ctx.check_stop();

            // 如果 checkpoint 已有首选商人，直接使用
            if (!ctx.checkpoint->preferred_difu_merchant.empty()) {
                state->bestMerchant = ctx.checkpoint->preferred_difu_merchant;
                BOT_LOG("TencentBot", "  使用首选地府商人: " << state->bestMerchant);
            } else {
                // 查询上商人价格
                ctx.merchants.walk_to_difu_upper();
                state->price1 = ctx.trade.query_buy_price("地府货商", "纸钱");
                if (state->price1 < 0) {
                    ctx.sleep_ms(timing().ui_update_delay_ms);
                    ctx.merchants.walk_to_difu_upper();
                    state->price1 = ctx.trade.query_buy_price("地府货商", "纸钱");
                }
                BOT_LOG("TencentBot", "  地府货商 纸钱买价="
                        << (state->price1 > 0 ? std::to_string(state->price1) : "失败"));

                if (state->price1 > 0 && state->price1 <= threshold().paper_buy_price_max) {
                    state->bestMerchant = "地府货商";
                } else {
                    // 查询下商人价格
                    ctx.merchants.walk_to_difu_lower();
                    state->price2 = ctx.trade.query_buy_price("地府商人", "纸钱");
                    if (state->price2 < 0) {
                        ctx.sleep_ms(timing().ui_update_delay_ms);
                        ctx.merchants.walk_to_difu_lower();
                        state->price2 = ctx.trade.query_buy_price("地府商人", "纸钱");
                    }
                    BOT_LOG("TencentBot", "  地府商人 纸钱买价="
                            << (state->price2 > 0 ? std::to_string(state->price2) : "失败"));

                    if (state->price1 > 0 &&
                        (state->price2 < 0 || state->price1 <= state->price2)) {
                        state->bestMerchant = "地府货商";
                    } else {
                        state->bestMerchant = "地府商人";
                    }
                }
                ctx.checkpoint->preferred_difu_merchant = state->bestMerchant;
                (void)ctx.store->save(*ctx.checkpoint);
            }

            state->otherMerchant =
                (state->bestMerchant == "地府货商") ? "地府商人" : "地府货商";
            return bt::Status::Success;
        },
        obs
    ));

    // --- 子节点 3: 买纸钱（带 fallback + 无限重试） ---
    // 构建: Selector(buy_best, buy_other)
    // 外层用 RetryDecorator 包装（30s 间隔无限重试）
    auto buyFromBest = std::make_unique<bt::Action>(
        "buy_paper_from_best",
        [&ctx, state]() -> bt::Status {
            if (ctx.check_stop) ctx.check_stop();
            if (state->bestMerchant == "地府货商") ctx.merchants.walk_to_difu_upper();
            else ctx.merchants.walk_to_difu_lower();

            if (ctx.trade.buy_item(state->bestMerchant, "纸钱")) {
                state->lastPaperBuyPrice = (state->price1 > 0) ? state->price1 :
                                           ((state->price2 > 0) ? state->price2 : 2700);
                BOT_LOG("TencentBot", "  购买成功，价格(估)=" << state->lastPaperBuyPrice);
                return bt::Status::Success;
            }
            BOT_WARN("TencentBot", "首选商人 " << state->bestMerchant << " 购买失败");
            return bt::Status::Failure;
        },
        obs
    );

    auto buyFromOther = std::make_unique<bt::Action>(
        "buy_paper_from_other",
        [&ctx, state]() -> bt::Status {
            if (ctx.check_stop) ctx.check_stop();
            if (state->otherMerchant == "地府货商") ctx.merchants.walk_to_difu_upper();
            else ctx.merchants.walk_to_difu_lower();

            if (ctx.trade.buy_item(state->otherMerchant, "纸钱")) {
                state->lastPaperBuyPrice = (state->price2 > 0) ? state->price2 :
                                           ((state->price1 > 0) ? state->price1 : 2700);
                BOT_LOG("TencentBot", "  备选购买成功，价格(估)=" << state->lastPaperBuyPrice);
                ctx.checkpoint->preferred_difu_merchant = state->otherMerchant;
                (void)ctx.store->save(*ctx.checkpoint);
                return bt::Status::Success;
            }
            BOT_WARN("TencentBot", "地府纸钱可能全卖完了");
            return bt::Status::Failure;
        },
        obs
    );

    std::vector<std::unique_ptr<bt::Node>> buy_fallback_children;
    buy_fallback_children.emplace_back(std::move(buyFromBest));
    buy_fallback_children.emplace_back(std::move(buyFromOther));
    auto buySelector = std::make_unique<bt::Selector>(
        "buy_paper_selector", std::move(buy_fallback_children), obs);

    // 外层 RetryDecorator: 全部失败后 30 秒重试，无限次
    auto buyWithRetry = std::make_unique<bt::RetryDecorator>(
        "buy_paper_retry",
        std::move(buySelector),
        -1,  // 无限重试
        std::chrono::milliseconds(30000),
        obs
    );

    seq_children.emplace_back(std::move(buyWithRetry));

    return std::make_unique<bt::Sequence>("difu_buy_paper", std::move(seq_children), obs);
}

// =============================================================================
// Step2: 地府 → … → 北俱泸州（8 段传送链）
// 原来的 8-phase 状态机，拆为 Sequence(8 个 Action)。
// =============================================================================
std::unique_ptr<bt::Node> BuildStep2_TravelToBeiju(
    TradingStepContext& ctx, bt::Observer obs
) {
    std::vector<std::unique_ptr<bt::Node>> children;
    children.reserve(9);

    children.emplace_back(std::make_unique<bt::Action>(
        "step2_log",
        [&ctx]() -> bt::Status {
            if (ctx.check_stop) ctx.check_stop();
            BOT_LOG("TencentBot", "[3/6] 地府 → … → 北俱泸州（8段传送）");
            return bt::Status::Success;
        },
        obs
    ));

    children.emplace_back(MakeVoidAction(
        "route_leave_difu", ctx.routes.leave_difu, ctx.check_stop, obs));
    children.emplace_back(MakeVoidAction(
        "route_datang_to_chishui", ctx.routes.datang_to_chishui, ctx.check_stop, obs));
    children.emplace_back(MakeVoidAction(
        "route_chishui_to_nvba", ctx.routes.chishui_to_nvba, ctx.check_stop, obs));
    children.emplace_back(MakeVoidAction(
        "route_nvba_to_donghaiyandong", ctx.routes.nvba_to_donghaiyandong, ctx.check_stop, obs));
    children.emplace_back(MakeVoidAction(
        "route_donghaiyandong_to_donghaiwan", ctx.routes.donghaiyandong_to_donghaiwan, ctx.check_stop, obs));
    children.emplace_back(MakeVoidAction(
        "route_donghaiwan_to_aolai", ctx.routes.donghaiwan_to_aolai, ctx.check_stop, obs));
    children.emplace_back(MakeVoidAction(
        "route_aolai_to_huaguo", ctx.routes.aolai_to_huaguo, ctx.check_stop, obs));
    children.emplace_back(MakeVoidAction(
        "route_huaguo_to_beiju", ctx.routes.huaguo_to_beiju, ctx.check_stop, obs));

    return std::make_unique<bt::Sequence>("travel_to_beiju", std::move(children), obs);
}

// =============================================================================
// Step3: 北俱：卖纸钱（比价）+ 买油
// 原来的 5-phase 状态机，拆为：
//   Sequence(compare_prices → sell_paper → buy_oil_with_retry)
// =============================================================================
std::unique_ptr<bt::Node> BuildStep3_BeijuSellPaperBuyOil(
    TradingStepContext& ctx, bt::Observer obs
) {
    struct State {
        int salePrice1 = -1;
        int salePrice2 = -1;
        std::string sellTarget;
        std::string buyTarget;
    };
    auto state = std::make_shared<State>();

    std::vector<std::unique_ptr<bt::Node>> seq_children;

    // --- 日志 ---
    seq_children.emplace_back(std::make_unique<bt::Action>(
        "step3_log",
        [&ctx]() -> bt::Status {
            if (ctx.check_stop) ctx.check_stop();
            BOT_LOG("TencentBot", "[4/6] 北俱: 卖纸钱 + 买油");
            return bt::Status::Success;
        },
        obs
    ));

    // --- 比价 ---
    seq_children.emplace_back(std::make_unique<bt::Action>(
        "beiju_compare_paper_sale_prices",
        [&ctx, state]() -> bt::Status {
            if (ctx.check_stop) ctx.check_stop();

            ctx.merchants.walk_to_beiju_upper();
            state->salePrice1 = ctx.trade.query_sale_price("北俱货商", "纸钱");
            if (state->salePrice1 < 0) {
                ctx.sleep_ms(timing().ui_update_delay_ms);
                ctx.merchants.walk_to_beiju_upper();
                state->salePrice1 = ctx.trade.query_sale_price("北俱货商", "纸钱");
            }
            BOT_LOG("TencentBot", "  北俱货商 纸钱收购价=" << state->salePrice1);

            ctx.merchants.walk_to_beiju_lower();
            state->salePrice2 = ctx.trade.query_sale_price("北俱商人", "纸钱");
            if (state->salePrice2 < 0) {
                ctx.sleep_ms(timing().ui_update_delay_ms);
                ctx.merchants.walk_to_beiju_lower();
                state->salePrice2 = ctx.trade.query_sale_price("北俱商人", "纸钱");
            }
            BOT_LOG("TencentBot", "  北俱商人 纸钱收购价=" << state->salePrice2);

            if (state->salePrice1 >= state->salePrice2) {
                state->sellTarget = "北俱货商";
                state->buyTarget = "北俱商人";
            } else {
                state->sellTarget = "北俱商人";
                state->buyTarget = "北俱货商";
            }
            BOT_LOG("TencentBot", "  决策: 卖给 " << state->sellTarget
                    << "，从 " << state->buyTarget << " 买油");
            return bt::Status::Success;
        },
        obs
    ));

    // --- 卖纸钱 ---
    seq_children.emplace_back(std::make_unique<bt::Action>(
        "beiju_sell_paper",
        [&ctx, state]() -> bt::Status {
            if (ctx.check_stop) ctx.check_stop();
            if (state->sellTarget == "北俱货商") ctx.merchants.walk_to_beiju_upper();
            else ctx.merchants.walk_to_beiju_lower();

            const int paperMoney = ctx.trade.sell_item(state->sellTarget, "纸钱");
            (void)ctx.try_return_if_reached("beiju_sale_paper", paperMoney);
            return bt::Status::Success;
        },
        obs
    ));

    // --- 买油（带 fallback + 重试）---
    auto buyOilFromBest = std::make_unique<bt::Action>(
        "buy_oil_from_best",
        [&ctx, state]() -> bt::Status {
            if (ctx.check_stop) ctx.check_stop();
            if (state->buyTarget == "北俱货商") ctx.merchants.walk_to_beiju_upper();
            else ctx.merchants.walk_to_beiju_lower();
            BOT_LOG("TencentBot", "  尝试向 " << state->buyTarget << " 买油");
            if (ctx.trade.buy_item(state->buyTarget, "油")) {
                return bt::Status::Success;
            }
            return bt::Status::Failure;
        },
        obs
    );

    auto buyOilFromOther = std::make_unique<bt::Action>(
        "buy_oil_from_other",
        [&ctx, state]() -> bt::Status {
            if (ctx.check_stop) ctx.check_stop();
            const std::string altTarget =
                (state->buyTarget == "北俱货商") ? "北俱商人" : "北俱货商";
            BOT_WARN("TencentBot", "首选买油商人 " << state->buyTarget
                    << " 缺货，尝试备选 " << altTarget);
            if (altTarget == "北俱货商") ctx.merchants.walk_to_beiju_upper();
            else ctx.merchants.walk_to_beiju_lower();
            if (ctx.trade.buy_item(altTarget, "油")) {
                return bt::Status::Success;
            }
            BOT_WARN("TencentBot", "北俱油可能全卖完了");
            return bt::Status::Failure;
        },
        obs
    );

    std::vector<std::unique_ptr<bt::Node>> buy_oil_children;
    buy_oil_children.emplace_back(std::move(buyOilFromBest));
    buy_oil_children.emplace_back(std::move(buyOilFromOther));
    auto buyOilSelector = std::make_unique<bt::Selector>(
        "buy_oil_selector", std::move(buy_oil_children), obs);

    auto buyOilWithRetry = std::make_unique<bt::RetryDecorator>(
        "buy_oil_retry",
        std::move(buyOilSelector),
        -1,  // 无限重试
        std::chrono::milliseconds(30000),
        obs
    );

    seq_children.emplace_back(std::move(buyOilWithRetry));

    return std::make_unique<bt::Sequence>(
        "beiju_sell_paper_buy_oil", std::move(seq_children), obs);
}

// =============================================================================
// Step4: 北俱 → 长安 → 大唐国境 → 地府
// 原来的 3-phase 状态机，拆为 Sequence(3 个 Action)。
// =============================================================================
std::unique_ptr<bt::Node> BuildStep4_ReturnToDifu(
    TradingStepContext& ctx, bt::Observer obs
) {
    std::vector<std::unique_ptr<bt::Node>> children;
    children.reserve(4);

    children.emplace_back(std::make_unique<bt::Action>(
        "step4_log",
        [&ctx]() -> bt::Status {
            if (ctx.check_stop) ctx.check_stop();
            BOT_LOG("TencentBot", "[5/6] 北俱 → 长安 → 大唐国境 → 地府");
            return bt::Status::Success;
        },
        obs
    ));

    children.emplace_back(MakeVoidAction(
        "route_beiju_to_changan", ctx.routes.beiju_to_changan, ctx.check_stop, obs));

    children.emplace_back(MakeVoidAction(
        "route_changan_to_datang_return",
        ctx.routes.changan_to_datang_for_return, ctx.check_stop, obs));

    children.emplace_back(MakeVoidAction(
        "route_datang_to_difu_return",
        ctx.routes.datang_to_difu_for_return, ctx.check_stop, obs));

    return std::make_unique<bt::Sequence>("return_to_difu", std::move(children), obs);
}

// =============================================================================
// Step5: 地府卖油
// 原来的 3-phase 状态机，拆为 Sequence(compare → sell)。
// =============================================================================
std::unique_ptr<bt::Node> BuildStep5_DifuSellOil(
    TradingStepContext& ctx, bt::Observer obs
) {
    struct State {
        int oilSalePrice1 = -1;
        int oilSalePrice2 = -1;
        std::string sellTarget;
        std::string nextBuyTarget;
    };
    auto state = std::make_shared<State>();

    std::vector<std::unique_ptr<bt::Node>> seq_children;

    // --- 日志 ---
    seq_children.emplace_back(std::make_unique<bt::Action>(
        "step5_log",
        [&ctx]() -> bt::Status {
            if (ctx.check_stop) ctx.check_stop();
            BOT_LOG("TencentBot", "[6/6] 地府卖油");
            return bt::Status::Success;
        },
        obs
    ));

    // --- 比价 ---
    seq_children.emplace_back(std::make_unique<bt::Action>(
        "difu_compare_oil_sale_prices",
        [&ctx, state]() -> bt::Status {
            if (ctx.check_stop) ctx.check_stop();

            ctx.merchants.walk_to_difu_upper();
            state->oilSalePrice1 = ctx.trade.query_sale_price("地府货商", "油");
            if (state->oilSalePrice1 < 0) {
                ctx.sleep_ms(timing().ui_update_delay_ms);
                ctx.merchants.walk_to_difu_upper();
                state->oilSalePrice1 = ctx.trade.query_sale_price("地府货商", "油");
            }
            BOT_LOG("TencentBot", "  地府货商 油收购价=" << state->oilSalePrice1);

            ctx.merchants.walk_to_difu_lower();
            state->oilSalePrice2 = ctx.trade.query_sale_price("地府商人", "油");
            if (state->oilSalePrice2 < 0) {
                ctx.sleep_ms(timing().ui_update_delay_ms);
                ctx.merchants.walk_to_difu_lower();
                state->oilSalePrice2 = ctx.trade.query_sale_price("地府商人", "油");
            }
            BOT_LOG("TencentBot", "  地府商人 油收购价=" << state->oilSalePrice2);

            if (state->oilSalePrice1 >= state->oilSalePrice2) {
                state->sellTarget = "地府货商";
                state->nextBuyTarget = "地府商人";
            } else {
                state->sellTarget = "地府商人";
                state->nextBuyTarget = "地府货商";
            }
            BOT_LOG("TencentBot", "  决策: 卖给 " << state->sellTarget
                    << "，下一轮首选买家更新为 " << state->nextBuyTarget);
            return bt::Status::Success;
        },
        obs
    ));

    // --- 卖油 ---
    seq_children.emplace_back(std::make_unique<bt::Action>(
        "difu_sell_oil_to_merchant",
        [&ctx, state]() -> bt::Status {
            if (ctx.check_stop) ctx.check_stop();
            if (state->sellTarget == "地府货商") ctx.merchants.walk_to_difu_upper();
            else ctx.merchants.walk_to_difu_lower();

            const int money = ctx.trade.sell_item(state->sellTarget, "油");
            (void)ctx.try_return_if_reached("difu_sale_oil", money);
            ctx.checkpoint->preferred_difu_merchant = state->nextBuyTarget;
            (void)ctx.store->save(*ctx.checkpoint);
            return bt::Status::Success;
        },
        obs
    ));

    return std::make_unique<bt::Sequence>("difu_sell_oil", std::move(seq_children), obs);
}

// =============================================================================
// Goal 分支: 达标后回帮派 + 提交银票
// 前置 Condition(is_goal_reached) + 地图感知路由 Selector
// =============================================================================
std::unique_ptr<bt::Node> BuildGoalBranch(
    TradingStepContext& ctx, bt::Observer obs
) {
    std::vector<std::unique_ptr<bt::Node>> goal_seq_children;
    goal_seq_children.reserve(2);

    // 条件: is_goal_reached
    goal_seq_children.emplace_back(std::make_unique<bt::Condition>(
        "goal_reached?",
        [&ctx]() -> bool {
            return ctx.checkpoint && ctx.checkpoint->is_goal_reached;
        },
        obs
    ));

    // 动作: 根据当前地图选择路由回帮派
    // 用 Selector 匹配当前地图，每个分支是 Sequence(Condition(map==X), Action(route))
    auto makeMapRoute = [&](const std::string& mapName, const std::string& nodeName,
                            std::function<void()> routeFn) -> std::unique_ptr<bt::Node> {
        std::vector<std::unique_ptr<bt::Node>> ch;
        ch.emplace_back(std::make_unique<bt::Condition>(
            "is_map_" + mapName,
            [&ctx, mapName]() -> bool {
                return ctx.get_current_map && ctx.get_current_map() == mapName;
            },
            obs
        ));
        ch.emplace_back(std::make_unique<bt::Action>(
            nodeName,
            [&ctx, routeFn = std::move(routeFn)]() -> bt::Status {
                if (ctx.check_stop) ctx.check_stop();
                routeFn();
                return bt::Status::Running; // 路线完成后仍需继续导航
            },
            obs
        ));
        return std::make_unique<bt::Sequence>("goal_from_" + mapName, std::move(ch), obs);
    };

    std::vector<std::unique_ptr<bt::Node>> goal_route_children;

    goal_route_children.emplace_back(makeMapRoute(
        "beijuluzhou", "goal_route_beiju_to_changan", ctx.routes.beiju_to_changan));

    goal_route_children.emplace_back(makeMapRoute(
        "difu", "goal_route_leave_difu", ctx.routes.leave_difu));

    goal_route_children.emplace_back(makeMapRoute(
        "datangguojing", "goal_route_datang_to_changan",
        ctx.routes.datang_to_changan_city));

    goal_route_children.emplace_back(makeMapRoute(
        "changancheng", "goal_route_changan_to_bangpai",
        ctx.routes.changan_to_bangpai));

    // 帮派/金库: 提交银票并完成
    {
        std::vector<std::unique_ptr<bt::Node>> final_ch;
        final_ch.emplace_back(std::make_unique<bt::Condition>(
            "is_map_bangpai_or_jinku",
            [&ctx]() -> bool {
                if (!ctx.get_current_map) return false;
                const std::string m = ctx.get_current_map();
                return m == "bangpai" || m == "jinku";
            },
            obs
        ));
        final_ch.emplace_back(std::make_unique<bt::Action>(
            "goal_submit_money",
            [&ctx]() -> bt::Status {
                if (ctx.check_stop) ctx.check_stop();
                if (ctx.process_idiom_verify) ctx.process_idiom_verify();
                BOT_LOG("TencentBot", "已返回帮派并提交银票，跑商完成");
                if (ctx.store) (void)ctx.store->clear();
                throw_goal_reached();
                return bt::Status::Success; // unreachable
            },
            obs
        ));
        goal_route_children.emplace_back(std::make_unique<bt::Sequence>(
            "goal_from_bangpai", std::move(final_ch), obs));
    }

    // 未知地图错误处理
    goal_route_children.emplace_back(std::make_unique<bt::Action>(
        "goal_unexpected_map",
        [&ctx]() -> bt::Status {
            if (!ctx.get_current_map) return bt::Status::Failure;
            const std::string curMap = ctx.get_current_map();
            BOT_ERR("TencentBot", "当前地图不在回城路径上(" << curMap
                    << ")，请手动回城完成最后提交");
            if (ctx.checkpoint) {
                ctx.checkpoint->last_op_name = "return_path_unexpected_map";
                if (ctx.store) (void)ctx.store->save(*ctx.checkpoint);
            }
            return bt::Status::Failure;
        },
        obs
    ));

    auto goal_router = std::make_unique<bt::Selector>(
        "goal_route_selector", std::move(goal_route_children), obs);

    goal_seq_children.emplace_back(std::move(goal_router));

    return std::make_unique<bt::Sequence>("goal_branch", std::move(goal_seq_children), obs);
}

} // namespace domain
