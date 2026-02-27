#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

// 统一内存后端接口：当前仅保留 vsock INIT_BIND 握手能力。
class IProcessMemoryReader {
public:
    virtual ~IProcessMemoryReader() = default;

    // 运行前初始化上下文。vsock 新架构在这里完成一次性 INIT_BIND 握手。
    virtual bool initialize_binding(std::uint64_t target_pid, std::string* error) {
        (void)target_pid;
        if (error) {
            error->clear();
        }
        return true;
    }

    // 是否使用 Host->Guest 共享结构体数据面（而非主动读远端内存）。
    virtual bool uses_shared_data_feed() const { return false; }
};

// 新方案：vsock 仅做 INIT_BIND 握手，后续数据面由 Host 物理写入共享结构体。
class VsockMemoryReader final : public IProcessMemoryReader {
public:
    VsockMemoryReader(std::uint32_t cid, std::uint32_t port, std::uint32_t timeout_ms);
    ~VsockMemoryReader() override;

    bool initialize_binding(std::uint64_t target_pid, std::string* error) override;
    bool uses_shared_data_feed() const override { return true; }

private:
    bool init_bind_once(std::uint64_t target_pid, std::string* error);
    bool connect_vsock(std::uintptr_t* out_sock, std::string* error);
    bool send_text(std::uintptr_t sock, const std::string& text, std::string* error);
    bool recv_text(std::uintptr_t sock, std::string* out_text, std::string* error);
    void close_socket(std::uintptr_t sock);

    std::uint32_t cid_;
    std::uint32_t port_;
    std::uint32_t timeout_ms_;
    std::mutex bind_mu_;
    bool bind_done_ = false;
};

std::shared_ptr<IProcessMemoryReader> create_vsock_memory_reader(
    std::uint32_t cid,
    std::uint32_t port,
    std::uint32_t timeout_ms
);
