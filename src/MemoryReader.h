#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

// 统一内存读取后端接口：GameMemory 只依赖此接口，不直接依赖 hv/vsock。
class IProcessMemoryReader {
public:
    virtual ~IProcessMemoryReader() = default;

    virtual bool read_virtual_by_pid(
        std::uint64_t pid,
        std::uint64_t va,
        void* out,
        std::size_t size,
        std::string* error
    ) = 0;

    virtual bool query_module_base_by_pid(
        std::uint64_t pid,
        const std::string& module_name,
        std::uint64_t* out_base,
        std::string* error
    ) = 0;
};

// 旧方案：Hypervisor + CR3 读 VA。
class HvMemoryReader final : public IProcessMemoryReader {
public:
    bool read_virtual_by_pid(
        std::uint64_t pid,
        std::uint64_t va,
        void* out,
        std::size_t size,
        std::string* error
    ) override;

    bool query_module_base_by_pid(
        std::uint64_t pid,
        const std::string& module_name,
        std::uint64_t* out_base,
        std::string* error
    ) override;

private:
    bool query_cr3(std::uint64_t pid, std::uint64_t* out_cr3, std::string* error);

    std::unordered_map<std::uint64_t, std::uint64_t> cr3_cache_;
};

// 新方案：通过 guestread vsock 协议读取（pid+va->pa 后读物理内存）。
class VsockMemoryReader final : public IProcessMemoryReader {
public:
    VsockMemoryReader(std::uint32_t cid, std::uint32_t port, std::uint32_t timeout_ms);
    ~VsockMemoryReader() override;

    bool read_virtual_by_pid(
        std::uint64_t pid,
        std::uint64_t va,
        void* out,
        std::size_t size,
        std::string* error
    ) override;

    bool query_module_base_by_pid(
        std::uint64_t pid,
        const std::string& module_name,
        std::uint64_t* out_base,
        std::string* error
    ) override;

private:
    bool ensure_connected(std::string* error);
    void close_socket();

    bool request_roundtrip(
        const void* req,
        std::size_t req_size,
        void* out_resp,
        std::size_t resp_size,
        std::uint8_t* out_data,
        std::size_t out_data_size,
        std::size_t* out_data_read,
        std::string* error
    );

    std::uint32_t cid_;
    std::uint32_t port_;
    std::uint32_t timeout_ms_;
    std::uintptr_t sock_ = static_cast<std::uintptr_t>(~std::uintptr_t{0});
};

std::shared_ptr<IProcessMemoryReader> create_hv_memory_reader();
std::shared_ptr<IProcessMemoryReader> create_vsock_memory_reader(
    std::uint32_t cid,
    std::uint32_t port,
    std::uint32_t timeout_ms
);
