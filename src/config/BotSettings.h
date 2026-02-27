#pragma once

#include "../domain/MapProperties.h"

#include <cstdint>
#include <map>
#include <string>

namespace config {

struct TimingSettings {
    int key_action_delay_ms = 100;
    int ui_update_delay_ms = 1000;
    int map_change_delay_ms = 1500;
    int ui_click_delay_ms = 1000;
    int trade_confirm_delay_ms = 1000;
};

struct WindowOffsetSettings {
    int x = -6;
    int y = -57;
};

struct TradeThresholdSettings {
    int paper_buy_price_max = 2600;
    int oil_buy_price_max = 3600;
    double paper_sell_profit_ratio = 1.50;
    double oil_sell_profit_ratio = 1.50;
};

// 进程启动相关参数：
// main.cpp 默认从这里读取，命令行参数仅用于覆盖。
struct RuntimeSettings {
    std::string checkpoint_path = "bot_checkpoint.json";
    std::string mem_backend = "vsock";
    std::uint32_t vsock_cid = 2;
    std::uint32_t vsock_port = 4050;
    std::uint32_t vsock_timeout_ms = 5000;
    std::uint32_t remote_port = 19090;
    std::uint32_t cursor_interval_ms = 100;
};

struct BotSettings {
    TimingSettings timing;
    WindowOffsetSettings window_offset;
    TradeThresholdSettings trade_threshold;
    RuntimeSettings runtime;
    std::map<std::string, MapProperties> map_properties;
};

// 读取全局配置（首次调用时懒加载；若配置文件缺失则使用内置默认值）。
const BotSettings& GetBotSettings();

} // namespace config
