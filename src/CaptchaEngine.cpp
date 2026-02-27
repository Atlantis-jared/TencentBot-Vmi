#include "CaptchaEngine.h"
#include <curl/curl.h>
#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

CaptchaEngine::CaptchaEngine(const std::string& serverUrl) : baseUrl(serverUrl) {
#ifdef _WIN32
    // 自动尝试设置控制台为 UTF-8 解决乱码
    SetConsoleOutputCP(CP_UTF8);
#endif
    curl_global_init(CURL_GLOBAL_ALL);
    CE_LOG("就绪 Server=" << baseUrl);
}

CaptchaEngine::~CaptchaEngine() {
    curl_global_cleanup();
}

int CaptchaEngine::recognizeMap(int width, int height, const std::vector<uint8_t>& bgraData) {
    std::string resp = postMultipart(baseUrl + "/recognize_map", encodeToPng(width, height, bgraData));

    try {
        // 2. 解析返回的 JSON
        auto j = json::parse(resp);

        // 3. 检查 success 字段并获取 map_index
        if (j.value("success", false)) {
            return j.value("map_index", -1);
        } else {
            std::string msg = j.value("msg", "unknown error");
            CE_ERR("Map Recognition Server Error: " << msg);
        }
    } catch (const std::exception& e) {
        CE_ERR("JSON Parse Error in recognizeMap: " << e.what());
    }

    return -1; // 失败返回 -1
}

std::vector<unsigned char> CaptchaEngine::encodeToPng(int w, int h, const std::vector<uint8_t>& data) {
    if (data.empty()) return {};
    try {
        cv::Mat frame(h, w, CV_8UC4, (void*)data.data());
        std::vector<unsigned char> buf;
        cv::imencode(".png", frame, buf);
        return buf;
    } catch (const std::exception& e) {
        CE_ERR("OpenCV Error: " << e.what());
        return {};
    }
}

size_t CaptchaEngine::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    ((std::string*)userp)->append((char*)contents, totalSize);
    return totalSize;
}

std::string CaptchaEngine::postMultipart(const std::string& url,
                                        const std::vector<unsigned char>& imgBuf,
                                        const std::map<std::string, std::string>& textFields) {
    if (imgBuf.empty()) return "";
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_data(part, (const char*)imgBuf.data(), imgBuf.size());
    curl_mime_filename(part, "frame.png");
    curl_mime_type(part, "image/png");

    for (auto const& [key, val] : textFields) {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, key.c_str());
        curl_mime_data(part, val.c_str(), CURL_ZERO_TERMINATED);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        CE_ERR("CURL failed: " << curl_easy_strerror(res));
    }

    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return response;
}

std::string CaptchaEngine::recognizeIdiom(int width, int height, const std::vector<uint8_t>& bgraData) {
    std::string resp = postMultipart(baseUrl + "/recognize", encodeToPng(width, height, bgraData));
    try {
        auto j = json::parse(resp);
        return j.value("final_idiom", "None");
    } catch (...) { return "ParseError"; }
}

std::string CaptchaEngine::recognizeNumber(int width, int height, const std::vector<uint8_t>& bgraData) {
    std::string resp = postMultipart(baseUrl + "/recognize_number", encodeToPng(width, height, bgraData));
    try {
        auto j = json::parse(resp);
        if (j.value("success", false))
            return j.value("numbers", "");
    } catch (...) {}
    return "";
}

std::vector<Point2D> CaptchaEngine::findChar(int width, int height, const std::vector<uint8_t>& bgraData, const std::string& target) {
    std::map<std::string, std::string> fields = { {"target", target} };
    std::string resp = postMultipart(baseUrl + "/find_char", encodeToPng(width, height, bgraData), fields);

    std::vector<Point2D> points;
    try {
        auto j = json::parse(resp);
        if (j.value("success", false)) {
            for (auto& item : j["data"]) {
                Point2D p{ item["center"][0].get<int>(), item["center"][1].get<int>(), item["score"].get<double>() };
                points.push_back(p);
            }
        }
    } catch (...) { CE_ERR("JSON Parse Error in findChar"); }
    return points;
}

std::vector<Point2D> CaptchaEngine::detectObject(int width, int height, const std::vector<uint8_t>& bgraData, const std::string& targetName) {
    std::map<std::string, std::string> fields = { {"target", targetName} };
    std::string resp = postMultipart(baseUrl + "/detect_object", encodeToPng(width, height, bgraData), fields);

    std::vector<Point2D> points;
    try {
        auto j = json::parse(resp);
        if (j.value("success", false)) {
            for (auto& item : j["data"]) {
                Point2D p{ item["center"][0].get<int>(), item["center"][1].get<int>(), item["confidence"].get<double>() };
                points.push_back(p);
            }
        }
    } catch (const std::exception& e) { CE_ERR("Parse Error: " << e.what()); }
    return points;
}
