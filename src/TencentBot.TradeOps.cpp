#include "TencentBot.h"

#include "config/BotSettings.h"
#include "domain/RunControl.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <windows.h>

#include <opencv2/core/mat.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {

const config::TimingSettings& timing() {
    return config::GetBotSettings().timing;
}

} // namespace

int TencentBot::readItemPrice(int tradeMode, const std::string& itemName) {
    int searchRegionX = (tradeMode == 0) ? 0 : 517;
    int searchRegionW = (tradeMode == 0) ? 517 : 294;

    vision.captureToBuffer();
    if (vision.imageBuffer.empty() || vision.frameW <= 0 || vision.frameH <= 0) {
        return -1;
    }

    cv::Mat fullScreenImg(vision.frameH, vision.frameW, CV_8UC4, vision.imageBuffer.data());
    if (searchRegionX >= vision.frameW) {
        return -2;
    }
    searchRegionW = std::min(searchRegionW, vision.frameW - searchRegionX);
    cv::Rect searchRect(searchRegionX, 0, searchRegionW, vision.frameH);
    cv::Mat searchRegionImg = fullScreenImg(searchRect).clone();

    std::vector<uint8_t> searchImgData(
        searchRegionImg.data,
        searchRegionImg.data + (searchRegionImg.total() * searchRegionImg.elemSize())
    );
    auto detectedPositions = aiCaptcha->detectObject(
        searchRegionImg.cols,
        searchRegionImg.rows,
        searchImgData,
        itemName
    );

    if (detectedPositions.empty()) {
        Point2D localMatch = vision.findNpcInScreenRegion(itemName, searchRegionX, 0, searchRegionW, vision.frameH);
        if (localMatch.score <= 0) {
            return -1;
        }
        clickUiElement(localMatch.x, localMatch.y);
    } else {
        clickUiElement(detectedPositions[0].x + searchRegionX, detectedPositions[0].y);
    }

    moveCharacterTo(200, 200, 0);
    domain::sleep_interruptible(stopSignal_, timing().ui_update_delay_ms);

    vision.captureToBuffer();
    cv::Mat freshImg(vision.frameH, vision.frameW, CV_8UC4, vision.imageBuffer.data());

    int priceRoiX = (tradeMode == 0) ? 386 : 657;
    if (priceRoiX + 125 > vision.frameW || 500 + 32 > vision.frameH) {
        return -2;
    }

    cv::Rect priceRect(priceRoiX, 500, 125, 32);
    cv::Mat priceRegionImg = freshImg(priceRect).clone();
    std::vector<uint8_t> priceImgData(
        priceRegionImg.data,
        priceRegionImg.data + (priceRegionImg.total() * priceRegionImg.elemSize())
    );
    std::string recognizedText = aiCaptcha->recognizeNumber(
        priceRegionImg.cols,
        priceRegionImg.rows,
        priceImgData
    );

    try {
        if (recognizedText.empty()) {
            return 0;
        }
        return std::stoi(recognizedText);
    } catch (...) {
        return -3;
    }
}

int TencentBot::readCurrentMoney() {
    moveCharacterTo(200, 70, 0);
    domain::sleep_interruptible(stopSignal_, timing().key_action_delay_ms);

    vision.captureToBuffer();
    if (vision.imageBuffer.empty()) {
        return -1;
    }

    cv::Mat fullImg(vision.frameH, vision.frameW, CV_8UC4, vision.imageBuffer.data());
    cv::Rect moneyRect(381, 589, 120, 40);
    moneyRect &= cv::Rect(0, 0, fullImg.cols, fullImg.rows);
    if (moneyRect.width <= 0 || moneyRect.height <= 0) {
        return -4;
    }

    cv::Mat moneyRegionImg = fullImg(moneyRect).clone();
    std::vector<uint8_t> moneyImgData(
        moneyRegionImg.data,
        moneyRegionImg.data + (moneyRegionImg.total() * moneyRegionImg.elemSize())
    );
    std::string recognizedText = aiCaptcha->recognizeNumber(
        moneyRegionImg.cols,
        moneyRegionImg.rows,
        moneyImgData
    );

    try {
        if (recognizedText.empty()) {
            return -2;
        }
        return std::stoi(recognizedText);
    } catch (...) {
        return -3;
    }
}

int TencentBot::queryNpcBuyPrice(const std::string& npcName, const std::string& itemName) {
    if (!clickNpcIfFound(npcName, 0, -40)) {
        return -1;
    }
    clickUiElement(242, 473);
    if (!waitForTradePanel()) {
        return -1;
    }

    int price = readItemPrice(0, itemName);
    auto cancelHits = vision.findNpcOnScreen("取消");
    if (!cancelHits.empty()) {
        clickUiElement(cancelHits[0].x, cancelHits[0].y);
    }
    return price;
}

int TencentBot::queryNpcSalePrice(const std::string& npcName, const std::string& itemName) {
    if (!clickNpcIfFound(npcName, 0, -40)) {
        return -1;
    }
    clickUiElement(242, 473);
    if (!waitForTradePanel()) {
        return -1;
    }

    int price = readItemPrice(1, itemName);
    auto cancelHits = vision.findNpcOnScreen("取消");
    if (!cancelHits.empty()) {
        clickUiElement(cancelHits[0].x, cancelHits[0].y);
    }
    return price;
}

bool TencentBot::waitForTradePanel() {
    for (int i = 0; i < 15; ++i) {
        domain::check_stop_or_throw(stopSignal_);
        auto hits = vision.findNpcOnScreen("交易面板");
        if (!hits.empty()) {
            return true;
        }
        domain::sleep_interruptible(stopSignal_, 200);
    }
    BOT_WARN("TencentBot", "等待交易面板超时");
    return false;
}

bool TencentBot::buyItemFromNpc(const std::string& npcName, const std::string& itemName) {
    domain::check_stop_or_throw(stopSignal_);
    if (!clickNpcIfFound(npcName, 0, -40)) {
        return false;
    }
    clickUiElement(242, 473);

    if (!waitForTradePanel()) {
        return false;
    }

    auto itemHits = vision.findNpcOnScreen(itemName);
    if (itemHits.empty()) {
        return false;
    }
    clickUiElement(itemHits[0].x, itemHits[0].y);

    Point2D maxQtyBtn = vision.findNpcInScreenRegion("最大数量", 0, 0, 517, 718);
    if (maxQtyBtn.score <= 0) {
        return false;
    }
    clickUiElement(maxQtyBtn.x, maxQtyBtn.y);

    Point2D confirmBtn = vision.findNpcInScreenRegion("确定", 0, 0, 517, 718);
    if (confirmBtn.score <= 0) {
        return false;
    }
    clickUiElement(confirmBtn.x, confirmBtn.y);

    clickUiElement(858, 430);

    auto cancelHits = vision.findNpcOnScreen("取消");
    if (!cancelHits.empty()) {
        clickUiElement(cancelHits[0].x, cancelHits[0].y);
    }

    BOT_LOG("TencentBot", "买入 " << itemName << " 完成");
    return true;
}

int TencentBot::sellItemToNpc(const std::string& npcName, const std::string& itemName) {
    domain::check_stop_or_throw(stopSignal_);
    constexpr int VERIFY_HALF_SIZE = 40;

    for (int attempt = 0; attempt < 2; ++attempt) {
        domain::check_stop_or_throw(stopSignal_);
        if (attempt > 0) {
            BOT_WARN("TencentBot", "卖出 " << itemName << " 重试 (" << (attempt + 1) << "/2)");
            auto cancelHits = vision.findNpcOnScreen("取消");
            if (!cancelHits.empty()) {
                clickUiElement(cancelHits[0].x, cancelHits[0].y);
            }
            domain::sleep_interruptible(stopSignal_, timing().ui_click_delay_ms);
        }

        if (!clickNpcIfFound(npcName, 0, -40)) {
            continue;
        }
        clickUiElement(242, 473);

        if (!waitForTradePanel()) {
            auto cancelHits = vision.findNpcOnScreen("取消");
            if (!cancelHits.empty()) {
                clickUiElement(cancelHits[0].x, cancelHits[0].y);
            }
            continue;
        }

        auto itemHits = vision.findNpcOnScreen(itemName);
        if (itemHits.empty()) {
            BOT_WARN("TencentBot", "卖出列表中未找到 " << itemName);
            continue;
        }
        int itemCenterX = itemHits[0].x;
        int itemCenterY = itemHits[0].y;
        clickUiElement(itemCenterX, itemCenterY);

        domain::check_stop_or_throw(stopSignal_);
        moveCharacterTo(200, 70, 0);
        Point2D maxQtyBtn = vision.findNpcInScreenRegion("最大数量", 517, 0, 294, 718);
        if (maxQtyBtn.score <= 0) {
            BOT_WARN("TencentBot", "未找到\"最大数量\"按钮");
            continue;
        }
        clickUiElement(maxQtyBtn.x, maxQtyBtn.y);

        Point2D confirmBtn = vision.findNpcInScreenRegion("确定", 517, 0, 294, 718);
        if (confirmBtn.score <= 0) {
            BOT_WARN("TencentBot", "未找到\"确定\"按钮");
            continue;
        }
        clickUiElement(confirmBtn.x, confirmBtn.y);
        domain::sleep_interruptible(stopSignal_, timing().trade_confirm_delay_ms);

        int verifyX = std::max(0, itemCenterX - VERIFY_HALF_SIZE);
        int verifyY = std::max(0, itemCenterY - VERIFY_HALF_SIZE);
        vision.captureToBuffer();
        Point2D verifyHit = vision.findNpcInScreenRegion(
            itemName,
            verifyX,
            verifyY,
            VERIFY_HALF_SIZE * 2,
            VERIFY_HALF_SIZE * 2
        );
        bool itemGone = (verifyHit.score <= 0);

        if (itemGone) {
            BOT_LOG("TencentBot", "卖出 " << itemName << " 成功（物品格子已清空）");
            int currentMoney = readCurrentMoney();
            BOT_LOG("TencentBot", "卖出后银票=" << currentMoney);
            auto cancelHits = vision.findNpcOnScreen("取消");
            if (!cancelHits.empty()) {
                clickUiElement(cancelHits[0].x, cancelHits[0].y);
            }
            return currentMoney;
        }

        BOT_WARN("TencentBot", "卖出 " << itemName << " 后仍检测到物品图标，视为失败");
        auto cancelHits = vision.findNpcOnScreen("取消");
        if (!cancelHits.empty()) {
            clickUiElement(cancelHits[0].x, cancelHits[0].y);
        }
    }

    BOT_ERR("TencentBot", "卖出 " << itemName << " 失败（多次尝试后格子里仍有该物品）");
    auto cancelHits = vision.findNpcOnScreen("取消");
    if (!cancelHits.empty()) {
        clickUiElement(cancelHits[0].x, cancelHits[0].y);
    }
    return -1;
}
