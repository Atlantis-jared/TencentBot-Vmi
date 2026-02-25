// =============================================================================
// VisionEngine.cpp — 视觉识别引擎实现
// =============================================================================
#include "VisionEngine.h"
#include "BotLogger.h"

#include <filesystem>
#include <algorithm>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace fs = std::filesystem;

// 地图识别匹配分数阈值：高于此值才认为识别成功
static constexpr double kMapRecognizeMinScore    = 0.80;
// 地图 UI 控件定位匹配分数阈值
static constexpr double kMapUiLocateMinScore     = 0.75;
// 地图模板目录（相对于可执行文件工作目录）
static constexpr const char* kMapTemplateDir     = "maps/";
// 地图 UI 模板目录
static constexpr const char* kMapUiTemplateDir   = "mapsui/";

// =============================================================================
// 构造函数
// =============================================================================
VisionEngine::VisionEngine(DxgiWindowCapture& screenCapture)
    : screenRef(screenCapture) {}

// =============================================================================
// loadAllTemplates() — 统一入口，初始化时调用一次
// =============================================================================
bool VisionEngine::loadAllTemplates() {
    BOT_LOG("VisionEngine", "开始加载所有模板...");
    loadMapTemplatesFromDisk();
    initNpcConfigs();
    loadNpcTemplatesFromDisk();
    BOT_LOG("VisionEngine", "模板加载完毕，地图模板=" << mapTemplates.size()
            << " NPC模板=" << npcTemplates.size());
    if (mapTemplates.empty()) {
        BOT_ERR("VisionEngine", "地图模板为空，初始化失败");
        return false;
    }
    if (npcTemplates.empty() || npcTemplates.size() != npcConfigs.size()) {
        BOT_ERR("VisionEngine", "NPC 模板加载不完整，初始化失败: loaded="
                << npcTemplates.size() << " expected=" << npcConfigs.size());
        return false;
    }
    return true;
}

// =============================================================================
// loadMapTemplatesFromDisk() — 读取 maps/ 目录下所有 .png 文件作为地图识别模板
// 文件名（不含扩展名）即为地图名，例如 "maps/changancheng.png" → key="changancheng"
// =============================================================================
void VisionEngine::loadMapTemplatesFromDisk() {
    mapTemplates.clear();

    if (!fs::exists(kMapTemplateDir)) {
        BOT_ERR("VisionEngine", "地图模板目录不存在: "
                << fs::absolute(kMapTemplateDir).string());
        return;
    }

    for (const auto& dirEntry : fs::directory_iterator(kMapTemplateDir)) {
        if (dirEntry.path().extension() != ".png") continue;

        // 读取彩色图（必须与 matchTemplate 保持相同通道数）
        cv::Mat templateImg = cv::imread(dirEntry.path().string(), cv::IMREAD_COLOR);
        if (templateImg.empty()) {
            BOT_WARN("VisionEngine", "地图模板读取失败（文件可能损坏）: " << dirEntry.path().string());
            continue;
        }

        const std::string mapName = dirEntry.path().stem().string();
        mapTemplates[mapName] = std::move(templateImg);
    }

    if (mapTemplates.empty()) {
        BOT_WARN("VisionEngine", "地图模板目录存在但未能加载任何 .png 文件");
    }
}

// =============================================================================
// initNpcConfigs() — 配置 NPC 模板路径和匹配阈值
// 新增 NPC 只需在此处追加一行即可，loadNpcTemplatesFromDisk() 会自动读取。
// =============================================================================
void VisionEngine::initNpcConfigs() {
    // 格式：npcConfigs["UI显示名"] = {图片路径, 匹配阈值}
    // 路径相对于可执行文件的工作目录
    npcConfigs["帮派主管"]   = {"npcs/bangpaizhuguan.png",        0.7};
    npcConfigs["驿站老板"]   = {"npcs/yizhanlaoban.png",          0.7};
    npcConfigs["传送北俱"]   = {"npcs/chuansongbeijutudi.png",    0.7};
    npcConfigs["传送傲来"]   = {"npcs/chuansongaolaichuanfu.png", 0.7};
    npcConfigs["传送守卫"]   = {"npcs/chuansongshouwei.png",      0.7};
    npcConfigs["白虎堂总管"] = {"npcs/baihutangzongguan.png",      0.7}; // 与传送守卫共用同一模板
    npcConfigs["纸钱"]       = {"npcs/zhiqian.png",               0.7};
    npcConfigs["油"]         = {"npcs/you.png",                   0.7};
    npcConfigs["地府商人"]   = {"npcs/difushangren.png",          0.7};
    npcConfigs["地府货商"]   = {"npcs/difuhuoshang.png",          0.7};
    npcConfigs["北俱商人"]   = {"npcs/beijushangren.png",         0.7};
    npcConfigs["北俱货商"]   = {"npcs/beijuhuoshang.png",         0.7};
    npcConfigs["取消"]       = {"npcs/quxiao.png",                0.7};
    npcConfigs["最大数量"]   = {"npcs/zuidashuliang.png",         0.7};
    npcConfigs["确定"]       = {"npcs/queding.png",               0.7};
    npcConfigs["交易面板"]   = {"npcs/jiaoyimianban.png",         0.7}; // 验证面板是否打开
    npcConfigs["确定给与"]   = {"npcs/quedinggeiyu.png",         0.7};
    npcConfigs["银票"]       = {"npcs/yinpiao.png",               0.7};
    npcConfigs["成语确认"]   = {"npcs/chengyuqueren.png",         0.7};
    npcConfigs["重置"]       = {"npcs/chongzhi.png",              0.7};
}

// =============================================================================
// loadNpcTemplatesFromDisk() — 按配置加载 NPC 模板图片到内存缓存
// =============================================================================
void VisionEngine::loadNpcTemplatesFromDisk() {
    npcTemplates.clear();
    for (const auto& [npcName, config] : npcConfigs) {
        cv::Mat templateImg = cv::imread(config.imagePath, cv::IMREAD_COLOR);
        if (templateImg.empty()) {
            BOT_ERR("VisionEngine", "NPC 模板文件缺失或损坏: " << config.imagePath
                    << "（NPC=「" << npcName << "」）");
            continue;
        }
        npcTemplates[npcName] = std::move(templateImg);
    }
}

// =============================================================================
// captureToBuffer() — 调用 DxgiWindowCapture 截取当前游戏窗口画面
// 结果写入 imageBuffer / frameW / frameH，返回是否成功
// =============================================================================
bool VisionEngine::captureToBuffer() {
    // 最多尝试两次（第一次失败时尝试重建捕获器）
    for (int trialIdx = 0; trialIdx < 2; ++trialIdx) {
        bool captureOk = screenRef.captureFrame(imageBuffer, frameW, frameH);
        if (captureOk && !imageBuffer.empty()) return true;

        if (trialIdx == 0 && screenRef.recreateIfNeeded()) {
            Sleep(500); // 等待 D3D 重建完成
            continue;
        }
        break;
    }
    return false;
}

// =============================================================================
// getCurrentMapName() — 识别当前屏幕所处的游戏地图
// 对每个地图模板做 TM_CCOEFF_NORMED 匹配，返回最高分的地图名。
// =============================================================================
std::string VisionEngine::getCurrentMapName() {
    if (mapTemplates.empty()) {
        BOT_WARN("VisionEngine", "无地图模板，无法识别当前地图");
        return "Unknown";
    }

    if (!captureToBuffer()) {
        BOT_WARN("VisionEngine", "截图失败，返回 CaptureEmpty");
        return "CaptureEmpty";
    }

    // 将 BGRA 截图转换为 BGR（与模板一致）
    cv::Mat screenBGRA(frameH, frameW, CV_8UC4, imageBuffer.data());
    cv::Mat screenBGR;
    cv::cvtColor(screenBGRA, screenBGR, cv::COLOR_BGRA2BGR);

    double bestMatchScore = 0.0;
    std::string bestMapName = "Unknown";

    for (const auto& [mapName, templateImg] : mapTemplates) {
        // 跳过比当前屏幕大的模板（matchTemplate 会抛出异常）
        if (templateImg.cols > screenBGR.cols || templateImg.rows > screenBGR.rows) continue;

        cv::Mat matchResult;
        cv::matchTemplate(screenBGR, templateImg, matchResult, cv::TM_CCOEFF_NORMED);

        double minScore, maxScore;
        cv::minMaxLoc(matchResult, &minScore, &maxScore);

        if (maxScore > bestMatchScore) {
            bestMatchScore = maxScore;
            bestMapName    = mapName;
        }
    }

    if (bestMatchScore >= kMapRecognizeMinScore) return bestMapName;

    BOT_WARN("VisionEngine", "地图识别分数偏低(" << bestMatchScore << " < "
             << kMapRecognizeMinScore << ")，返回 Unknown");
    return "Unknown";
}

// =============================================================================
// locateMapUiOnScreen() — 定位小地图 UI 控件在屏幕上的位置
// 同时尝试原名 + 原名+"1" 两个模板（昼夜双图），取分数更高的。
// =============================================================================
MapUiLocation VisionEngine::locateMapUiOnScreen(const std::string& mapName) {
    if (!captureToBuffer()) {
        return {cv::Point(0, 0), 0, false};
    }

    cv::Mat screenBGRA(frameH, frameW, CV_8UC4, imageBuffer.data());
    cv::Mat screenBGR;
    cv::cvtColor(screenBGRA, screenBGR, cv::COLOR_BGRA2BGR);

    // 尝试两个模板变体：标准版和夜间版（文件名后缀 "1"）
    const std::vector<std::string> templateVariants = {"", "1"};

    double    bestScore     = -1.0;
    cv::Point bestTopLeft   = {0, 0};
    int       bestHeight    = 0;

    for (const auto& variantSuffix : templateVariants) {
        const std::string cacheKey  = mapName + variantSuffix;
        const std::string imagePath = std::string(kMapUiTemplateDir) + cacheKey + ".png";

        // 懒加载：首次使用时从磁盘读入缓存
        if (mapUiTemplateCache.find(cacheKey) == mapUiTemplateCache.end()) {
            // 先检查文件是否存在，避免 cv::imread 对缺失文件输出 OpenCV 警告
            if (!fs::exists(imagePath)) continue; // 该变体不存在，静默跳过
            cv::Mat uiTemplate = cv::imread(imagePath, cv::IMREAD_COLOR);
            if (uiTemplate.empty()) continue;
            mapUiTemplateCache[cacheKey] = std::move(uiTemplate);
        }

        const cv::Mat& uiTemplate = mapUiTemplateCache[cacheKey];
        cv::Mat matchResult;
        cv::matchTemplate(screenBGR, uiTemplate, matchResult, cv::TM_CCOEFF_NORMED);

        double maxScore;
        cv::Point maxLoc;
        cv::minMaxLoc(matchResult, nullptr, &maxScore, nullptr, &maxLoc);

        if (maxScore > bestScore) {
            bestScore   = maxScore;
            bestTopLeft = maxLoc;
            bestHeight  = uiTemplate.rows;
        }
    }

    if (bestScore > kMapUiLocateMinScore) {
        return {bestTopLeft, bestHeight, true};
    }

    BOT_WARN("VisionEngine", "小地图 UI 定位失败（地图=" << mapName
             << " 分数=" << bestScore << "）");
    return {cv::Point(0, 0), 0, false};
}

// =============================================================================
// findNpcOnScreen() — 全屏查找 NPC，返回最佳匹配位置（最多 1 个）
// =============================================================================
std::vector<Point2D> VisionEngine::findNpcOnScreen(const std::string& npcName) {
    std::vector<Point2D> resultList;

    if (npcTemplates.find(npcName) == npcTemplates.end()) {
        BOT_WARN("VisionEngine", "findNpcOnScreen: NPC「" << npcName << "」模板未加载");
        return resultList;
    }

    if (!captureToBuffer()) return resultList;

    // 转换色彩空间：BGRA → BGR（与模板一致）
    cv::Mat screenBGRA(frameH, frameW, CV_8UC4, imageBuffer.data());
    cv::Mat screenBGR;
    cv::cvtColor(screenBGRA, screenBGR, cv::COLOR_BGRA2BGR);

    const cv::Mat& npcTemplate   = npcTemplates.at(npcName);
    const double   scoreThreshold = npcConfigs.at(npcName).matchThreshold;

    cv::Mat matchResult;
    cv::matchTemplate(screenBGR, npcTemplate, matchResult, cv::TM_CCOEFF_NORMED);

    double    bestScore;
    cv::Point bestMatchLoc;
    cv::minMaxLoc(matchResult, nullptr, &bestScore, nullptr, &bestMatchLoc);

    if (bestScore >= scoreThreshold) {
        // 返回模板中心点（相对于全屏的坐标）
        Point2D hitPoint{};
        hitPoint.x     = bestMatchLoc.x + npcTemplate.cols / 2;
        hitPoint.y     = bestMatchLoc.y + npcTemplate.rows / 2;
        hitPoint.score = bestScore;
        resultList.push_back(hitPoint);
    }

    return resultList;
}

// =============================================================================
// findNpcInScreenRegion() — 在指定矩形区域内查找 NPC
// 返回坐标为全屏坐标（已加回 regionX/Y 偏移），未找到时 score <= 0。
// =============================================================================
Point2D VisionEngine::findNpcInScreenRegion(const std::string& npcName,
                                             int regionX, int regionY,
                                             int regionW, int regionH) {
    Point2D notFoundResult = {-1, -1, 0.0};

    if (npcTemplates.find(npcName) == npcTemplates.end()) {
        BOT_WARN("VisionEngine", "findNpcInScreenRegion: NPC「" << npcName << "」模板未加载");
        return notFoundResult;
    }

    if (!captureToBuffer()) return notFoundResult;

    // 转换截图色彩空间
    cv::Mat screenBGRA(frameH, frameW, CV_8UC4, imageBuffer.data());
    cv::Mat screenBGR;
    cv::cvtColor(screenBGRA, screenBGR, cv::COLOR_BGRA2BGR);

    // 裁剪出 ROI 区域，并做边界保护（防止传入超出屏幕范围的参数导致崩溃）
    cv::Rect searchRegion(regionX, regionY, regionW, regionH);
    searchRegion &= cv::Rect(0, 0, screenBGR.cols, screenBGR.rows); // 与屏幕矩形取交集
    if (searchRegion.empty()) return notFoundResult;

    cv::Mat regionImg = screenBGR(searchRegion).clone();

    const cv::Mat& npcTemplate    = npcTemplates.at(npcName);
    const double   scoreThreshold = npcConfigs.at(npcName).matchThreshold;

    // 模板尺寸检查：模板不能大于搜索区域
    if (npcTemplate.cols > regionImg.cols || npcTemplate.rows > regionImg.rows)
        return notFoundResult;

    cv::Mat matchResult;
    cv::matchTemplate(regionImg, npcTemplate, matchResult, cv::TM_CCOEFF_NORMED);

    double    bestScore;
    cv::Point bestMatchLocInRegion;
    cv::minMaxLoc(matchResult, nullptr, &bestScore, nullptr, &bestMatchLocInRegion);

    if (bestScore < scoreThreshold) return notFoundResult;

    // 将 ROI 内相对坐标转换回全屏绝对坐标
    Point2D result{};
    result.x     = searchRegion.x + bestMatchLocInRegion.x + npcTemplate.cols / 2;
    result.y     = searchRegion.y + bestMatchLocInRegion.y + npcTemplate.rows / 2;
    result.score = bestScore;
    return result;
}
