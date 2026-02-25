// DxgiWindowCapture.cpp
#include "DxgiWindowCapture.h"
#include <dwmapi.h>
#include <stdio.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

DxgiWindowCapture::DxgiWindowCapture()
{
    ZeroMemory(&lastRect, sizeof(lastRect));
    ZeroMemory(&outputDesc, sizeof(outputDesc));
}

DxgiWindowCapture::~DxgiWindowCapture()
{
    release();
}

HWND DxgiWindowCapture::findWindowByPid(DWORD pid)
{
    struct EnumData {
        DWORD pid;
        HWND hwnd;
    } data{ pid, nullptr };

    EnumWindows([](HWND hWnd, LPARAM lParam) -> BOOL {
        auto* d = reinterpret_cast<EnumData*>(lParam);

        DWORD winPid = 0;
        GetWindowThreadProcessId(hWnd, &winPid);

        if (winPid == d->pid && IsWindowVisible(hWnd)) {
            LONG style = GetWindowLong(hWnd, GWL_STYLE);
            if ((style & WS_CHILD) == 0) {
                d->hwnd = hWnd;
                return FALSE;
            }
        }
        return TRUE;
    }, (LPARAM)&data);

    return data.hwnd;
}

bool DxgiWindowCapture::initByPid(DWORD pid)
{
    savedPid = pid;
    targetHwnd = findWindowByPid(pid);

    if (!targetHwnd) {
        fprintf(stderr, "[Capture] 未找到窗口 PID=%lu\n", pid);
        return false;
    }

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device,
        &featureLevel,
        &context
    );

    if (FAILED(hr)) {
        fprintf(stderr, "[Capture] D3D11 设备创建失败 0x%08X\n", hr);
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    hr = device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr)) {
        fprintf(stderr, "[Capture] 获取 DXGI 设备失败 0x%08X\n", hr);
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();

    if (FAILED(hr)) {
        fprintf(stderr, "[Capture] 获取适配器失败 0x%08X\n", hr);
        return false;
    }

    HMONITOR hMonitor = MonitorFromWindow(targetHwnd, MONITOR_DEFAULTTONEAREST);

    IDXGIOutput* output = nullptr;
    IDXGIOutput* targetOutput = nullptr;
    UINT outputIndex = 0;

    while (adapter->EnumOutputs(outputIndex, &output) != DXGI_ERROR_NOT_FOUND) {
        DXGI_OUTPUT_DESC desc;
        output->GetDesc(&desc);
        if (desc.Monitor == hMonitor) {
            targetOutput = output;
            outputDesc = desc;
            outputDescValid = true;
            break;
        }

        output->Release();
        outputIndex++;
    }

    adapter->Release();

    if (!targetOutput) {
        fprintf(stderr, "[Capture] 未找到窗口所在显示器输出\n");
        return false;
    }

    IDXGIOutput1* output1 = nullptr;
    hr = targetOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
    targetOutput->Release();

    if (FAILED(hr)) {
        fprintf(stderr, "[Capture] 获取 IDXGIOutput1 失败 0x%08X\n", hr);
        return false;
    }

    hr = output1->DuplicateOutput(device, &duplication);
    output1->Release();

    if (FAILED(hr)) {
        fprintf(stderr, "[Capture] 创建桌面复制失败 0x%08X\n", hr);
        return false;
    }
    return true;
}

bool DxgiWindowCapture::updateTextures(int width, int height)
{
    if (cropTex) {
        cropTex->Release();
        cropTex = nullptr;
    }
    if (stagingTex) {
        stagingTex->Release();
        stagingTex = nullptr;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &cropTex);
    if (FAILED(hr)) {
        fprintf(stderr, "[Capture] 创建裁剪纹理失败 0x%08X\n", hr);
        return false;
    }

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = device->CreateTexture2D(&desc, nullptr, &stagingTex);
    if (FAILED(hr)) {
        fprintf(stderr, "[Capture] 创建 Staging 纹理失败 0x%08X\n", hr);
        if (cropTex) {
            cropTex->Release();
            cropTex = nullptr;
        }
        return false;
    }

    return true;
}

bool DxgiWindowCapture::captureFrame(std::vector<uint8_t>& data, int& width, int& height)
{
    if (!duplication || !targetHwnd) {
        data.clear();
        return false;
    }

    // 检查窗口状态
    if (!IsWindowVisible(targetHwnd) || IsIconic(targetHwnd)) {
        data.clear();
        return false;
    }

    // Windows 11：检查窗口是否被 cloaked
    BOOL isCloaked = FALSE;
    HRESULT hrCloaked = DwmGetWindowAttribute(
        targetHwnd,
        DWMWA_CLOAKED,
        &isCloaked,
        sizeof(isCloaked)
    );

    if (SUCCEEDED(hrCloaked) && isCloaked) {
        data.clear();
        return false;
    }

    // 获取窗口位置和大小
    RECT winRect = {};
    HRESULT hr = DwmGetWindowAttribute(
        targetHwnd,
        DWMWA_EXTENDED_FRAME_BOUNDS,
        &winRect,
        sizeof(winRect)
    );

    if (FAILED(hr)) {
        if (!GetWindowRect(targetHwnd, &winRect)) {
            data.clear();
            return false;
        }
    }

    // 转换为显示器相对坐标
    if (!outputDescValid) {
        fprintf(stderr, "[Capture] 显示器描述无效\n");
        data.clear();
        return false;
    }

    int captureLeft = winRect.left - outputDesc.DesktopCoordinates.left;
    int captureTop = winRect.top - outputDesc.DesktopCoordinates.top;
    int captureRight = winRect.right - outputDesc.DesktopCoordinates.left;
    int captureBottom = winRect.bottom - outputDesc.DesktopCoordinates.top;

    // 边界检查
    int desktopWidth = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
    int desktopHeight = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

    if (captureLeft < 0) captureLeft = 0;
    if (captureTop < 0) captureTop = 0;
    if (captureRight > desktopWidth) captureRight = desktopWidth;
    if (captureBottom > desktopHeight) captureBottom = desktopHeight;

    // 🔧 修复：重新计算宽高（在边界检查之后）
    width = captureRight - captureLeft;
    height = captureBottom - captureTop;

    if (width <= 0 || height <= 0) {
        fprintf(stderr, "[Capture] 裁剪后窗口尺寸无效 %dx%d\n", width, height);
        data.clear();
        return false;
    }

    // 获取桌面帧
    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    IDXGIResource* resource = nullptr;

    hr = duplication->AcquireNextFrame(500, &frameInfo, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        data.clear();
        return false;
    }

    if (FAILED(hr)) {
        // 🔧 修复：避免递归调用，使用标志位让外部重试
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            fprintf(stderr, "[Capture] 复制句柄丢失，需重新初始化\n");
            needsRecreate = true;
            release();
        }
        data.clear();
        return false;
    }

    // 🔧 修复：使用 RAII 或确保在所有路径释放资源
    ID3D11Texture2D* desktopTex = nullptr;
    hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTex);
    resource->Release();

    if (FAILED(hr)) {
        duplication->ReleaseFrame();
        data.clear();
        return false;
    }

    // 🔧 修复：使用自动清理确保资源释放
    struct AutoCleanup {
        ID3D11Texture2D* tex;
        IDXGIOutputDuplication* dup;
        ~AutoCleanup() {
            if (tex) tex->Release();
            if (dup) dup->ReleaseFrame();
        }
    } cleanup{ desktopTex, duplication };

    // 获取桌面纹理描述
    D3D11_TEXTURE2D_DESC desktopDesc;
    desktopTex->GetDesc(&desktopDesc);

    // 🔧 修复：检查窗口大小变化（使用实际裁剪后的大小）
    bool sizeChanged = !cropTex || !stagingTex ||
                       (lastWidth != width) || (lastHeight != height);

    if (sizeChanged) {
        if (!updateTextures(width, height)) {
            data.clear();
            return false;
        }
        lastWidth = width;
        lastHeight = height;
    }

    lastRect = winRect;

    // 设置裁剪框
    D3D11_BOX box = {};
    box.left = static_cast<UINT>(captureLeft);
    box.top = static_cast<UINT>(captureTop);
    box.right = static_cast<UINT>(captureRight);
    box.bottom = static_cast<UINT>(captureBottom);
    box.front = 0;
    box.back = 1;

    // 验证裁剪区域
    if (box.right > desktopDesc.Width) {
        box.right = desktopDesc.Width;
    }
    if (box.bottom > desktopDesc.Height) {
        box.bottom = desktopDesc.Height;
    }

    if (box.left >= box.right || box.top >= box.bottom) {
        fprintf(stderr, "[Capture] 裁剪区域无效 (%u,%u)-(%u,%u)\n",
                box.left, box.top, box.right, box.bottom);
        data.clear();
        return false;
    }

    // GPU端裁剪
    context->CopySubresourceRegion(
        cropTex,
        0,
        0, 0, 0,
        desktopTex,
        0,
        &box
    );

    // 复制到staging纹理
    context->CopyResource(stagingTex, cropTex);

    // 映射到CPU内存
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped);

    if (FAILED(hr)) {
        fprintf(stderr, "[Capture] 映射 Staging 纹理失败 0x%08X\n", hr);
        data.clear();
        return false;
    }

    // 分配输出缓冲区
    size_t bufferSize = static_cast<size_t>(width) * height * 4;
    data.resize(bufferSize);

    // 复制像素数据
    for (int y = 0; y < height; y++) {
        memcpy(
            data.data() + y * width * 4,
            static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch,
            width * 4
        );
    }

    context->Unmap(stagingTex, 0);

    // AutoCleanup 析构函数会自动释放资源
    return true;
}

// 🔧 新增：重建函数（外部调用）
bool DxgiWindowCapture::recreateIfNeeded()
{
    if (!needsRecreate) {
        return true;
    }

    needsRecreate = false;

    if (retryCount >= MAX_RETRY) {
        fprintf(stderr, "[Capture] 重建已达最大重试次数\n");
        retryCount = 0;
        return false;
    }

    retryCount++;
    if (initByPid(savedPid)) {
        retryCount = 0;
        return true;
    }

    return false;
}

void DxgiWindowCapture::release()
{
    if (stagingTex) {
        stagingTex->Release();
        stagingTex = nullptr;
    }

    if (cropTex) {
        cropTex->Release();
        cropTex = nullptr;
    }

    if (duplication) {
        duplication->Release();
        duplication = nullptr;
    }

    if (context) {
        context->Release();
        context = nullptr;
    }

    if (device) {
        device->Release();
        device = nullptr;
    }

    targetHwnd = nullptr;
    outputDescValid = false;
    lastWidth = 0;
    lastHeight = 0;
    ZeroMemory(&lastRect, sizeof(lastRect));
    ZeroMemory(&outputDesc, sizeof(outputDesc));
}