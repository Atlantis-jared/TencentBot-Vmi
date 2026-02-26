#include "BotSettings.h"

#include "../BotLogger.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace config {
namespace {

using json = nlohmann::json;

std::map<std::string, MapProperties> default_map_properties() {
    return {
        {"jianyecheng",     {556, 277, 288, 144}},
        {"donghaiwan",      {276, 276, 119, 119}},
        {"changancheng",    {545, 276, 548, 278}},
        {"difu",            {369, 276, 159, 119}},
        {"huaguoshan",      {369, 276, 159, 119}},
        {"nvbamu",          {220, 230,  95, 143}},
        {"donghaiyandong",  {550, 258, 191,  89}},
        {"aolaiguo",        {410, 276, 223, 150}},
        {"datangguojing",   {377, 360, 351, 335}},
        {"chishuizhou",     {510, 303, 161,  95}},
        {"beijuluzhou",     {367, 276, 227, 169}},
    };
}

BotSettings default_settings() {
    BotSettings settings{};
    settings.map_properties = default_map_properties();
    return settings;
}

void load_timing(const json& j, BotSettings* out) {
    if (!out || !j.is_object()) return;
    out->timing.key_action_delay_ms = j.value("key_action_delay_ms", out->timing.key_action_delay_ms);
    out->timing.ui_update_delay_ms = j.value("ui_update_delay_ms", out->timing.ui_update_delay_ms);
    out->timing.map_change_delay_ms = j.value("map_change_delay_ms", out->timing.map_change_delay_ms);
    out->timing.ui_click_delay_ms = j.value("ui_click_delay_ms", out->timing.ui_click_delay_ms);
    out->timing.trade_confirm_delay_ms = j.value("trade_confirm_delay_ms", out->timing.trade_confirm_delay_ms);
}

void load_window_offset(const json& j, BotSettings* out) {
    if (!out || !j.is_object()) return;
    out->window_offset.x = j.value("x", out->window_offset.x);
    out->window_offset.y = j.value("y", out->window_offset.y);
}

void load_trade_threshold(const json& j, BotSettings* out) {
    if (!out || !j.is_object()) return;
    out->trade_threshold.paper_buy_price_max =
        j.value("paper_buy_price_max", out->trade_threshold.paper_buy_price_max);
    out->trade_threshold.oil_buy_price_max =
        j.value("oil_buy_price_max", out->trade_threshold.oil_buy_price_max);
    out->trade_threshold.paper_sell_profit_ratio =
        j.value("paper_sell_profit_ratio", out->trade_threshold.paper_sell_profit_ratio);
    out->trade_threshold.oil_sell_profit_ratio =
        j.value("oil_sell_profit_ratio", out->trade_threshold.oil_sell_profit_ratio);
}

void load_map_properties(const json& j, BotSettings* out) {
    if (!out || !j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const json& row = it.value();
        if (!row.is_object()) continue;
        MapProperties prop{};
        prop.uiPixelWidth = row.value("uiPixelWidth", 0);
        prop.uiPixelHeight = row.value("uiPixelHeight", 0);
        prop.gameCoordMaxX = row.value("gameCoordMaxX", 0);
        prop.gameCoordMaxY = row.value("gameCoordMaxY", 0);
        if (prop.uiPixelWidth <= 0 || prop.uiPixelHeight <= 0 ||
            prop.gameCoordMaxX <= 0 || prop.gameCoordMaxY <= 0) {
            continue;
        }
        out->map_properties[it.key()] = prop;
    }
}

BotSettings load_from_file(const std::filesystem::path& path) {
    BotSettings settings = default_settings();
    std::ifstream in(path);
    if (!in) {
        BOT_WARN("BotConfig", "配置文件不存在，使用默认配置: " << path.string());
        return settings;
    }

    try {
        json root;
        in >> root;
        if (!root.is_object()) {
            BOT_WARN("BotConfig", "配置根节点非对象，使用默认配置");
            return settings;
        }
        load_timing(root.value("timing", json::object()), &settings);
        load_window_offset(root.value("window_offset", json::object()), &settings);
        load_trade_threshold(root.value("trade_threshold", json::object()), &settings);
        load_map_properties(root.value("map_properties", json::object()), &settings);
        BOT_LOG("BotConfig", "配置加载成功: " << path.string());
    } catch (const std::exception& e) {
        BOT_WARN("BotConfig", "配置解析失败，使用默认配置: " << e.what());
    }
    return settings;
}

} // namespace

const BotSettings& GetBotSettings() {
    static const BotSettings settings = [] {
        // 兼容两种运行方式：
        // 1) 在工程根目录直接运行（使用 assets/config）
        // 2) 在构建输出目录运行（CMake 会拷贝到 config）
        const std::filesystem::path p1 = "assets/config/bot_settings.json";
        if (std::filesystem::exists(p1)) {
            return load_from_file(p1);
        }
        const std::filesystem::path p2 = "config/bot_settings.json";
        if (std::filesystem::exists(p2)) {
            return load_from_file(p2);
        }
        BOT_WARN("BotConfig", "未找到配置文件，使用默认配置");
        return default_settings();
    }();
    return settings;
}

} // namespace config
