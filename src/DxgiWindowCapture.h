// DxgiWindowCapture.h
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vector>
#include <windows.h>

class DxgiWindowCapture
{
public:
    DxgiWindowCapture();
    ~DxgiWindowCapture();

    bool initByPid(DWORD pid);
    bool captureFrame(std::vector<uint8_t>& data, int& width, int& height);
    bool recreateIfNeeded();  // 🔧 新增：外部重建函数
    void release();

private:
    HWND findWindowByPid(DWORD pid);
    bool updateTextures(int width, int height);

    HWND targetHwnd = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGIOutputDuplication* duplication = nullptr;
    ID3D11Texture2D* cropTex = nullptr;
    ID3D11Texture2D* stagingTex = nullptr;
    RECT lastRect = {};

    // 🔧 新增：记录上次的宽高
    int lastWidth = 0;
    int lastHeight = 0;

    // Windows 11 兼容性相关
    DXGI_OUTPUT_DESC outputDesc = {};
    bool outputDescValid = false;
    DWORD savedPid = 0;
    int retryCount = 0;
    bool needsRecreate = false;  // 🔧 新增：重建标志
    static const int MAX_RETRY = 3;
};