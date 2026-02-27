#pragma once
#include <cstdint>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

#pragma pack(push, 1)
struct MemflowSharedPayload {
    uint32_t sync_flag;       // 0: idle, 1: active, 2: closing
    uint64_t timestamp_us;    // Host side timestamp
    int32_t  pit_x;
    int32_t  pit_y;
    int32_t  map_id;
    int32_t  role_raw_x;
    int32_t  role_raw_y;
};
#pragma pack(pop)

class MemflowConnector {
public:
    MemflowConnector();
    ~MemflowConnector();

    // 启动连接线程（非阻塞）
    bool start(uint32_t targetPid, uint64_t dllBase);
    void stop();

    const MemflowSharedPayload* getBuffer() const { return &buffer; }
    bool isActive() const { return buffer.sync_flag == 1; }

private:
    void threadMain(uint32_t targetPid, uint64_t dllBase);

    MemflowSharedPayload buffer;
    std::atomic_bool stopFlag;
    std::thread workerThread;
};
