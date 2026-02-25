#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "MemoryReader.h"

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <sstream>

#include "BotLogger.h"
#include "../hv.h"

#pragma comment(lib, "Ws2_32.lib")

namespace {

#ifndef AF_VSOCK
#define AF_VSOCK 40
#endif

constexpr std::uint32_t kOpRead = 1;
constexpr std::uint32_t kOpDllBase = 2;
constexpr std::uint32_t kOpVa2Pa = 3;
constexpr std::uint32_t kOpReadPhys = 4;
constexpr std::size_t kNameMax = 64;
constexpr std::size_t kMsgMax = 160;

struct sockaddr_vm {
    ADDRESS_FAMILY svm_family;
    std::uint16_t svm_reserved1;
    std::uint32_t svm_port;
    std::uint32_t svm_cid;
    std::uint8_t svm_zero[4];
};

struct RpcRequest {
    std::uint32_t op;
    std::uint32_t pid;
    std::uint64_t addr;
    std::uint32_t len;
    std::uint32_t reserved;
    char process_name[kNameMax];
    char module_name[kNameMax];
};

struct RpcResponse {
    std::int32_t status;
    std::uint32_t op;
    std::uint32_t data_len;
    std::uint32_t reserved;
    std::uint64_t value_u64;
    char message[kMsgMax];
};

static_assert(sizeof(RpcRequest) == 152, "RpcRequest layout mismatch");
static_assert(sizeof(RpcResponse) == 184, "RpcResponse layout mismatch");

std::string win_error_message(DWORD error_code) {
    LPSTR msg_buf = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD len = FormatMessageA(
        flags,
        nullptr,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&msg_buf),
        0,
        nullptr
    );
    if (len == 0 || msg_buf == nullptr) {
        std::ostringstream oss;
        oss << "Win32Error(" << error_code << ")";
        return oss.str();
    }
    std::string msg(msg_buf, len);
    LocalFree(msg_buf);
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' ' || msg.back() == '\t')) {
        msg.pop_back();
    }
    return msg;
}

std::string fixed_to_string(const char* src, std::size_t len) {
    std::size_t n = 0;
    while (n < len && src[n] != '\0') {
        ++n;
    }
    return std::string(src, n);
}

bool send_all(SOCKET sock, const std::uint8_t* data, std::size_t len, std::string* error) {
    std::size_t sent = 0;
    while (sent < len) {
        const int rc = send(sock, reinterpret_cast<const char*>(data + sent), static_cast<int>(len - sent), 0);
        if (rc == SOCKET_ERROR) {
            if (error) {
                *error = "send failed: " + win_error_message(static_cast<DWORD>(WSAGetLastError()));
            }
            return false;
        }
        if (rc == 0) {
            if (error) {
                *error = "send returned 0";
            }
            return false;
        }
        sent += static_cast<std::size_t>(rc);
    }
    return true;
}

bool recv_all(SOCKET sock, std::uint8_t* data, std::size_t len, std::string* error) {
    std::size_t done = 0;
    while (done < len) {
        const int rc = recv(sock, reinterpret_cast<char*>(data + done), static_cast<int>(len - done), 0);
        if (rc == SOCKET_ERROR) {
            if (error) {
                *error = "recv failed: " + win_error_message(static_cast<DWORD>(WSAGetLastError()));
            }
            return false;
        }
        if (rc == 0) {
            if (error) {
                *error = "peer closed connection";
            }
            return false;
        }
        done += static_cast<std::size_t>(rc);
    }
    return true;
}

class WsInitGuard {
public:
    WsInitGuard() {
        WSADATA wsa{};
        const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
        ok_ = (rc == 0);
    }
    ~WsInitGuard() {
        if (ok_) {
            WSACleanup();
        }
    }
    bool ok() const { return ok_; }
private:
    bool ok_ = false;
};

WsInitGuard& global_wsa() {
    static WsInitGuard guard;
    return guard;
}

}  // namespace

bool HvMemoryReader::query_cr3(std::uint64_t pid, std::uint64_t* out_cr3, std::string* error) {
    if (out_cr3 == nullptr) {
        if (error) {
            *error = "out_cr3 is null";
        }
        return false;
    }
    auto it = cr3_cache_.find(pid);
    if (it != cr3_cache_.end()) {
        *out_cr3 = it->second;
        return true;
    }
    const std::uint64_t cr3 = hv::query_process_cr3(pid);
    if (cr3 == 0) {
        if (error) {
            *error = "query_process_cr3 returned 0";
        }
        return false;
    }
    cr3_cache_[pid] = cr3;
    *out_cr3 = cr3;
    return true;
}

bool HvMemoryReader::read_virtual_by_pid(
    std::uint64_t pid,
    std::uint64_t va,
    void* out,
    std::size_t size,
    std::string* error
) {
    if (out == nullptr || size == 0) {
        if (error) {
            *error = "invalid output buffer";
        }
        return false;
    }
    std::uint64_t cr3 = 0;
    if (!query_cr3(pid, &cr3, error)) {
        return false;
    }
    hv::read_virt_mem(cr3, out, reinterpret_cast<void*>(va), size);
    return true;
}

bool HvMemoryReader::query_module_base_by_pid(
    std::uint64_t pid,
    const std::string& module_name,
    std::uint64_t* out_base,
    std::string* error
) {
    if (out_base == nullptr) {
        if (error) {
            *error = "out_base is null";
        }
        return false;
    }
    std::wstring wmodule(module_name.begin(), module_name.end());
    std::uint64_t base = hv::get_va_of_dllbase(pid, const_cast<wchar_t*>(wmodule.c_str()), 20);
    if (base == 0) {
        if (error) {
            *error = "get_va_of_dllbase returned 0";
        }
        return false;
    }
    *out_base = base;
    return true;
}

VsockMemoryReader::VsockMemoryReader(std::uint32_t cid, std::uint32_t port, std::uint32_t timeout_ms)
    : cid_(cid), port_(port), timeout_ms_(timeout_ms) {}

VsockMemoryReader::~VsockMemoryReader() {
    close_socket();
}

bool VsockMemoryReader::ensure_connected(std::string* error) {
    if (!global_wsa().ok()) {
        if (error) {
            *error = "WSAStartup failed";
        }
        return false;
    }

    const SOCKET invalid = INVALID_SOCKET;
    SOCKET current = static_cast<SOCKET>(sock_);
    if (current != invalid) {
        return true;
    }

    SOCKET sock = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        if (error) {
            *error = "socket(AF_VSOCK) failed: " + win_error_message(static_cast<DWORD>(WSAGetLastError()));
        }
        return false;
    }

    const DWORD tv = timeout_ms_;
    if (timeout_ms_ > 0) {
        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv)) == SOCKET_ERROR ||
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv)) == SOCKET_ERROR) {
            if (error) {
                *error = "setsockopt timeout failed: " + win_error_message(static_cast<DWORD>(WSAGetLastError()));
            }
            closesocket(sock);
            return false;
        }
    }

    sockaddr_vm addr{};
    addr.svm_family = AF_VSOCK;
    addr.svm_port = port_;
    addr.svm_cid = cid_;
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        if (error) {
            *error = "connect failed: " + win_error_message(static_cast<DWORD>(WSAGetLastError()));
        }
        closesocket(sock);
        return false;
    }

    sock_ = static_cast<std::uintptr_t>(sock);
    BOT_LOG("MemoryReader", "vsock connected cid=" << cid_ << " port=" << port_);
    return true;
}

void VsockMemoryReader::close_socket() {
    const SOCKET invalid = INVALID_SOCKET;
    SOCKET s = static_cast<SOCKET>(sock_);
    if (s != invalid) {
        closesocket(s);
        sock_ = static_cast<std::uintptr_t>(invalid);
    }
}

bool VsockMemoryReader::request_roundtrip(
    const void* req,
    std::size_t req_size,
    void* out_resp,
    std::size_t resp_size,
    std::uint8_t* out_data,
    std::size_t out_data_size,
    std::size_t* out_data_read,
    std::string* error
) {
    if (req == nullptr || out_resp == nullptr) {
        if (error) {
            *error = "invalid roundtrip pointers";
        }
        return false;
    }
    if (!ensure_connected(error)) {
        return false;
    }

    SOCKET sock = static_cast<SOCKET>(sock_);
    if (!send_all(sock, reinterpret_cast<const std::uint8_t*>(req), req_size, error) ||
        !recv_all(sock, reinterpret_cast<std::uint8_t*>(out_resp), resp_size, error)) {
        close_socket();
        return false;
    }

    if (out_data_read) {
        *out_data_read = 0;
    }

    const RpcResponse* resp = reinterpret_cast<const RpcResponse*>(out_resp);
    const std::size_t data_len = static_cast<std::size_t>(resp->data_len);
    if (data_len == 0) {
        return true;
    }
    if (out_data == nullptr || out_data_size < data_len) {
        if (error) {
            *error = "response data buffer is too small";
        }
        close_socket();
        return false;
    }
    if (!recv_all(sock, out_data, data_len, error)) {
        close_socket();
        return false;
    }
    if (out_data_read) {
        *out_data_read = data_len;
    }
    return true;
}

bool VsockMemoryReader::read_virtual_by_pid(
    std::uint64_t pid,
    std::uint64_t va,
    void* out,
    std::size_t size,
    std::string* error
) {
    if (pid == 0 || out == nullptr || size == 0 || size > std::numeric_limits<std::uint32_t>::max()) {
        if (error) {
            *error = "invalid read_virtual_by_pid arguments";
        }
        return false;
    }

    RpcRequest va2pa_req{};
    va2pa_req.op = kOpVa2Pa;
    va2pa_req.pid = static_cast<std::uint32_t>(pid);
    va2pa_req.addr = va;
    va2pa_req.len = static_cast<std::uint32_t>(size);

    RpcResponse va2pa_resp{};
    if (!request_roundtrip(&va2pa_req, sizeof(va2pa_req), &va2pa_resp, sizeof(va2pa_resp), nullptr, 0, nullptr, error)) {
        return false;
    }
    if (va2pa_resp.status != 0) {
        if (error) {
            *error = "va2pa failed: " + fixed_to_string(va2pa_resp.message, sizeof(va2pa_resp.message));
        }
        return false;
    }

    RpcRequest read_req{};
    read_req.op = kOpReadPhys;
    read_req.addr = va2pa_resp.value_u64;
    read_req.len = static_cast<std::uint32_t>(size);

    RpcResponse read_resp{};
    std::size_t data_read = 0;
    if (!request_roundtrip(
            &read_req,
            sizeof(read_req),
            &read_resp,
            sizeof(read_resp),
            reinterpret_cast<std::uint8_t*>(out),
            size,
            &data_read,
            error)) {
        return false;
    }
    if (read_resp.status != 0) {
        if (error) {
            *error = "read-phys failed: " + fixed_to_string(read_resp.message, sizeof(read_resp.message));
        }
        return false;
    }
    if (data_read != size) {
        if (error) {
            *error = "read-phys size mismatch";
        }
        return false;
    }
    return true;
}

bool VsockMemoryReader::query_module_base_by_pid(
    std::uint64_t pid,
    const std::string& module_name,
    std::uint64_t* out_base,
    std::string* error
) {
    if (pid == 0 || out_base == nullptr || module_name.empty()) {
        if (error) {
            *error = "invalid query_module_base_by_pid arguments";
        }
        return false;
    }

    RpcRequest req{};
    req.op = kOpDllBase;
    req.pid = static_cast<std::uint32_t>(pid);
    const std::size_t n = std::min(module_name.size(), kNameMax - 1);
    std::memcpy(req.module_name, module_name.data(), n);

    RpcResponse resp{};
    if (!request_roundtrip(&req, sizeof(req), &resp, sizeof(resp), nullptr, 0, nullptr, error)) {
        return false;
    }
    if (resp.status != 0) {
        if (error) {
            *error = "dllbase failed: " + fixed_to_string(resp.message, sizeof(resp.message));
        }
        return false;
    }
    *out_base = resp.value_u64;
    return true;
}

std::shared_ptr<IProcessMemoryReader> create_hv_memory_reader() {
    return std::make_shared<HvMemoryReader>();
}

std::shared_ptr<IProcessMemoryReader> create_vsock_memory_reader(
    std::uint32_t cid,
    std::uint32_t port,
    std::uint32_t timeout_ms
) {
    return std::make_shared<VsockMemoryReader>(cid, port, timeout_ms);
}

