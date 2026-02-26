#include "TencentBot.h"

#include "config/BotSettings.h"
#include "domain/RunControl.h"

#include <windows.h>

#include <string>

namespace {

const config::TimingSettings& timing() {
    return config::GetBotSettings().timing;
}

} // namespace

void TencentBot::route_changan_to_datangguojing() {
    BOT_LOG("TencentBot", "路线: 长安城 → 大唐国境");
    walkToPosition("changancheng", 273, 44);
    tryNpcTeleport("驿站老板", 0, -65, 263, 473, "datangguojing", "changancheng", 273, 44);
}

void TencentBot::route_datangguojing_to_difu() {
    BOT_LOG("TencentBot", "路线: 大唐国境 → 地府");
    walkToPosition("datangguojing", 46, 324);
    tryUiTeleport(584, 75, 6, 57, "difu", "datangguojing", 46, 324);
}

void TencentBot::route_leaveDisfu() {
    BOT_LOG("TencentBot", "路线: 地府 → 大唐国境（出地府）");
    walkToPosition("difu", 141, 4);
    clickUiElement(751, 709, 6, 57);
    Sleep(timing().ui_update_delay_ms);
}

void TencentBot::route_datangguojing_to_chishuizhou() {
    BOT_LOG("TencentBot", "路线: 大唐国境 → 赤水洲");
    walkToPosition("datangguojing", 203, 298);
    tryNpcTeleport("传送守卫", 0, -90, 251, 472, "chishuizhou", "datangguojing", 203, 298);
}

void TencentBot::route_datangguojing_to_changancheng() {
    BOT_LOG("TencentBot", "路线: 大唐国境 → 长安城");
    walkToPosition("datangguojing", 91, 255);
    tryNpcTeleport("驿站老板", 0, -50, 263, 473, "changancheng", "datangguojing", 91, 255);
}

void TencentBot::route_chishuizhou_to_nvbamu() {
    BOT_LOG("TencentBot", "路线: 赤水洲 → 女魃墓");
    walkToPosition("chishuizhou", 100, 77);
    tryUiTeleport(547, 295, 6, 57, "nvbamu", "chishuizhou", 100, 77);
}

void TencentBot::route_nvbamu_to_donghaiyandong() {
    BOT_LOG("TencentBot", "路线: 女魃墓 → 东海岩洞");
    walkToPosition("nvbamu", 13, 5);
    tryUiTeleport(124, 600, 6, 57, "donghaiyandong", "nvbamu", 13, 5);
}

void TencentBot::route_donghaiyandong_to_donghaiwan() {
    BOT_LOG("TencentBot", "路线: 东海岩洞 → 东海湾");
    walkToPosition("donghaiyandong", 85, 5);
    tryUiTeleport(384, 705, 6, 57, "donghaiwan", "donghaiyandong", 85, 5);
}

void TencentBot::route_donghaiwan_to_aolaiguo() {
    BOT_LOG("TencentBot", "路线: 东海湾 → 傲来国");
    walkToPosition("donghaiwan", 68, 18);
    Sleep(timing().ui_update_delay_ms);
    tryNpcTeleport("传送傲来", -5, -65, 256, 471, "aolaiguo", "donghaiwan", 68, 18);
}

void TencentBot::route_aolaiguo_to_huaguoshan() {
    BOT_LOG("TencentBot", "路线: 傲来国 → 花果山");
    walkToPosition("aolaiguo", 210, 141);
    tryUiTeleport(871, 88, 6, 57, "huaguoshan", "aolaiguo", 210, 141);
}

void TencentBot::route_huaguoshan_to_beijuluzhou() {
    BOT_LOG("TencentBot", "路线: 花果山 → 北俱泸州");
    walkToPosition("huaguoshan", 19, 97);
    Sleep(timing().ui_update_delay_ms);
    tryNpcTeleport("传送北俱", -5, -50, 269, 471, "beijuluzhou", "huaguoshan", 19, 97);
}

void TencentBot::route_beijuluzhou_to_changan() {
    BOT_LOG("TencentBot", "路线: 北俱泸州 → 长安城");
    walkToPosition("beijuluzhou", 42, 117);
    Sleep(timing().ui_update_delay_ms);
    tryNpcTeleport("驿站老板", 0, -60, 269, 471, "changancheng", "beijuluzhou", 42, 117);
}

void TencentBot::route_changan_to_bangpai() {
    BOT_LOG("TencentBot", "路线: 长安城 → 帮派");

    for (int retry = 1; retry <= 3; ++retry) {
        domain::check_stop_or_throw(stopSignal_);

        walkToPosition("changancheng", 386, 260);
        Sleep(timing().ui_update_delay_ms);
        if (!clickNpcIfFound("帮派主管", 0, -50)) {
            BOT_WARN("TencentBot", "未找到帮派主管，尝试 " << retry << "/3");
            continue;
        }

        clickUiElement(256, 473);
        Sleep(timing().ui_update_delay_ms);
        clickUiElement(1009, 352, 6, 57);
        Sleep(timing().ui_update_delay_ms);
        Sleep(2000);
        clickUiElement(831, 338, 6, 57);
        Sleep(timing().ui_update_delay_ms);
        Sleep(2000);
        clickUiElement(336, 246, 6, 57);
        Sleep(timing().ui_update_delay_ms);
        Sleep(2000);

        std::string curMapName = vision.getCurrentMapName();
        if (curMapName == "bangpai" || curMapName == "jinku") {
            BOT_LOG("TencentBot", "成功进入帮派/金库 (当前地图: " << curMapName << ")");
            return;
        }

        BOT_WARN("TencentBot", "未能进入帮派，重试 " << retry << "/3");
        Sleep(timing().ui_update_delay_ms);
    }

    BOT_ERR("TencentBot", "多次尝试进入帮派失败，请手动检查");
}

void TencentBot::route_leaveBangpai() {
    BOT_LOG("TencentBot", "出帮派 → 长安城");

    std::string curMap = vision.getCurrentMapName();

    if (curMap == "jinku") {
        BOT_LOG("TencentBot", "识别到金库 UI，点击离开...");
        clickUiElement(824, 708, 6, 57);
        Sleep(2000);
        Sleep(timing().ui_update_delay_ms);
        curMap = vision.getCurrentMapName();
    }

    if (curMap == "bangpai") {
        BOT_LOG("TencentBot", "识别到帮派 UI，点击离开...");
        clickUiElement(7, 521, 6, 57);
        Sleep(2000);
        Sleep(timing().ui_update_delay_ms);
    }

    if (curMap == "Unknown") {
        BOT_WARN("TencentBot", "当前地图 Unknown，尝试按金库/帮派顺序盲点离开...");
        clickUiElement(824, 708, 6, 57);
        Sleep(1000);
        clickUiElement(7, 521, 6, 57);
        Sleep(timing().ui_update_delay_ms);
    }
}

void TencentBot::walkToDifuLowerMerchant() {
    walkToPosition("difu", 92, 10);
}

void TencentBot::walkToDifuUpperMerchant() {
    walkToPosition("difu", 70, 44);
}

void TencentBot::walkToBeixuUpperMerchant() {
    walkToPosition("beijuluzhou", 164, 112);
}

void TencentBot::walkToBeixuLowerMerchant() {
    walkToPosition("beijuluzhou", 160, 40);
}

void TencentBot::resumeRoute_leaveBangpaiToDifu() {
    std::string currentMap = vision.getCurrentMapName();
    BOT_LOG("TencentBot", "恢复路线: 帮派→地府, 当前地图=" << currentMap);

    if (currentMap == "bangpai") {
        route_leaveBangpai();
        route_changan_to_datangguojing();
        route_datangguojing_to_difu();
    } else if (currentMap == "changancheng") {
        route_changan_to_datangguojing();
        route_datangguojing_to_difu();
    } else if (currentMap == "datangguojing") {
        route_datangguojing_to_difu();
    } else if (currentMap == "difu") {
        BOT_LOG("TencentBot", "已在地府，跳过恢复路线");
    } else {
        BOT_ERR("TencentBot", "无法恢复 leave_bangpai_to_difu，当前地图=" << currentMap);
    }
}

void TencentBot::resumeRoute_travelToBeixu() {
    std::string currentMap = vision.getCurrentMapName();
    BOT_LOG("TencentBot", "恢复路线: 地府→北俱, 当前地图=" << currentMap);

    if (currentMap == "difu") {
        route_leaveDisfu();
        currentMap = "datangguojing";
    }
    if (currentMap == "datangguojing") {
        route_datangguojing_to_chishuizhou();
        currentMap = "chishuizhou";
    }
    if (currentMap == "chishuizhou") {
        route_chishuizhou_to_nvbamu();
        currentMap = "nvbamu";
    }
    if (currentMap == "nvbamu") {
        route_nvbamu_to_donghaiyandong();
        currentMap = "donghaiyandong";
    }
    if (currentMap == "donghaiyandong") {
        route_donghaiyandong_to_donghaiwan();
        currentMap = "donghaiwan";
    }
    if (currentMap == "donghaiwan") {
        route_donghaiwan_to_aolaiguo();
        currentMap = "aolaiguo";
    }
    if (currentMap == "aolaiguo") {
        route_aolaiguo_to_huaguoshan();
        currentMap = "huaguoshan";
    }
    if (currentMap == "huaguoshan") {
        route_huaguoshan_to_beijuluzhou();
        currentMap = "beijuluzhou";
    }

    if (currentMap == "beijuluzhou") {
        BOT_LOG("TencentBot", "已到达北俱泸州");
    } else {
        BOT_ERR("TencentBot", "无法恢复 travel_to_beiju，当前地图=" << currentMap);
    }
}
