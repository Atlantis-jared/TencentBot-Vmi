#pragma once

#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

struct Point2D {
    int x;
    int y;
    double score;
};

class CaptchaEngine {
public:
    explicit CaptchaEngine(const std::string& serverUrl);
    ~CaptchaEngine();

    std::string recognizeIdiom(int width, int height, const std::vector<uint8_t>& bgraData);
    std::string recognizeNumber(int width, int height, const std::vector<uint8_t>& bgraData);
    std::vector<Point2D> findChar(int width, int height, const std::vector<uint8_t>& bgraData, const std::string& target);
    std::vector<Point2D> detectObject(int width, int height, const std::vector<uint8_t>& bgraData, const std::string& targetName);
    int recognizeMap(int width, int height, const std::vector<uint8_t>& bgraData);
private:
    std::string baseUrl;
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
    std::vector<unsigned char> encodeToPng(int w, int h, const std::vector<uint8_t>& data);
    std::string postMultipart(const std::string& url,
                             const std::vector<unsigned char>& imgBuf,
                             const std::map<std::string, std::string>& textFields = {});

public:
    static std::string currentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %X");
        return ss.str();
    }
};

#define CE_LOG(msg) std::cout << "[" << CaptchaEngine::currentTimestamp() << "] [INFO] " << msg << std::endl
#define CE_ERR(msg) std::cerr << "[" << CaptchaEngine::currentTimestamp() << "] [ERROR] " << msg << std::endl