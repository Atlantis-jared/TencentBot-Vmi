#include "TencentBot.h"

#include "config/BotSettings.h"
#include "domain/RunControl.h"

#include "../IbInputSimulator/Simulator/include/IbInputSimulator/InputSimulator.hpp"

namespace {

const config::TimingSettings& timing() {
    return config::GetBotSettings().timing;
}

} // namespace

void TencentBot::process_idiom_verify() {
    BOT_LOG("TencentBot", "开始成语验证流程...");

    // 1. 寻找总管
    auto npc_hits = vision.findNpcOnScreen("白虎堂总管");
    if (npc_hits.empty()) {
        BOT_ERR("TencentBot", "未找到「白虎堂总管」，请确认是否在金库门口");
        return;
    }
    Point2D pos_baihutangzongguan = npc_hits[0];

    // 2. 给予操作 (Alt+G)
    Send::KeyboardModifiers m = {0};
    m.LAlt = true;
    IbSendKeybdDownUp('G', m);
    Sleep(timing().ui_click_delay_ms);

    // 点击总管弹出给予界面
    clickUiElement(pos_baihutangzongguan.x, pos_baihutangzongguan.y, 0, -50);
    moveCharacterTo(100, 100, 0); // 移走鼠标避免遮挡
    Sleep(timing().ui_update_delay_ms);

    // 3. 选中银票
    auto v_yinpiao = vision.findNpcOnScreen("银票");
    if (v_yinpiao.empty()) {
        BOT_ERR("TencentBot", "给予界面中未找到「银票」");
        return;
    }
    clickUiElement(v_yinpiao[0].x, v_yinpiao[0].y);
    Sleep(timing().ui_click_delay_ms);

    // 4. 点击确定给予
    auto v_confirm = vision.findNpcOnScreen("确定给与");
    if (v_confirm.empty()) {
        BOT_ERR("TencentBot", "未找到「确定给与」按钮");
        return;
    }
    clickUiElement(v_confirm[0].x, v_confirm[0].y);
    moveCharacterTo(100, 100, 0); 
    Sleep(timing().ui_update_delay_ms * 3); // 等待验证码弹出

    // 5-7. 成语识别 + 点击 + 确认（支持答错重试）
    constexpr int kMaxCaptchaRetries = 3;
    for (int attempt = 1; attempt <= kMaxCaptchaRetries; ++attempt) {
        domain::check_stop_or_throw(stopSignal_);
        BOT_LOG("TencentBot", "成语验证尝试 " << attempt << "/" << kMaxCaptchaRetries);

        // 5. 识别成语
        vision.captureToBuffer();
        if (vision.imageBuffer.empty()) return;

        std::string idiom = aiCaptcha->recognizeIdiom(vision.frameW, vision.frameH, vision.imageBuffer);
        if (idiom.empty()) {
            BOT_ERR("TencentBot", "无法识别成语");
            return;
        }
        BOT_LOG("TencentBot", "目标成语: " << idiom);

        // 将 UTF-8 字符串拆分为单字列表
        std::vector<std::string> targetChars;
        for (size_t i = 0; i < idiom.length(); ) {
            unsigned char c = static_cast<unsigned char>(idiom[i]);
            int len = 1;
            if (c >= 0xf0) len = 4;
            else if (c >= 0xe0) len = 3;
            else if (c >= 0xc0) len = 2;
            targetChars.push_back(idiom.substr(i, len));
            i += len;
        }

        // 6. 按序点击汉字
        bool clickFailed = false;
        for (size_t i = 0; i < targetChars.size(); ++i) {
            domain::check_stop_or_throw(stopSignal_);
            std::string charToClick = targetChars[i];

            // 统计该字在成语中是第几次出现 (用于处理叠词，比如"抽抽噎噎")
            int occurrence = 0;
            for (size_t j = 0; j < i; ++j) {
                if (targetChars[j] == charToClick) occurrence++;
            }

            // 每次点击都要重新截图，因为界面会刷新
            vision.captureToBuffer();
            auto charPositions = aiCaptcha->findChar(vision.frameW, vision.frameH, vision.imageBuffer, charToClick);

            if (charPositions.size() <= static_cast<size_t>(occurrence)) {
                BOT_ERR("TencentBot", "未找到汉字「" << charToClick << "」的第 " << (occurrence + 1) << " 个坐标");
                clickFailed = true;
                break;
            }
            Point2D targetPos = charPositions[occurrence];
            BOT_LOG("TencentBot", "点击 [ " << charToClick << " ] (" << (i + 1) << "/4)");
            clickUiElement(targetPos.x, targetPos.y);
            moveCharacterTo(100, 100, 0);
            Sleep(timing().ui_update_delay_ms);
        }

        // 7. 点击确认按钮
        if (!clickFailed) {
            auto v_chengyuqueren = vision.findNpcOnScreen("成语确认");
            if (!v_chengyuqueren.empty()) {
                clickUiElement(v_chengyuqueren[0].x, v_chengyuqueren[0].y);
                moveCharacterTo(100, 100, 0);
                Sleep(timing().ui_update_delay_ms * 2); // 等待服务器判定结果
            } else {
                BOT_WARN("TencentBot", "未找到「成语确认」按钮");
            }
        }

        // 8. 检查是否答对：如果"重置"按钮不存在，说明答对了
        auto v_reset = vision.findNpcOnScreen("重置");
        if (v_reset.empty()) {
            BOT_LOG("TencentBot", "成语验证通过！");
            break;
        }
        // 答错了，点击重置并重试
        BOT_WARN("TencentBot", "成语答案错误，点击重置 (尝试 " << attempt << "/" << kMaxCaptchaRetries << ")");
        clickUiElement(v_reset[0].x, v_reset[0].y);
        moveCharacterTo(100, 100, 0);
        Sleep(timing().ui_update_delay_ms * 2); // 等待界面刷新后重新出题
    }
    BOT_LOG("TencentBot", "成语验证流程结束");
}
