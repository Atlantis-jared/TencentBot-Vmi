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
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>

#include "BotLogger.h"
#include "GameMemory.h"
#include "SharedDataStatus.h"
#include "../hv.h"

#pragma comment(lib, "Ws2_32.lib")

namespace {

#ifndef AF_VSOCK
#define AF_VSOCK 40
#endif

constexpr std::size_t kHandshakeReplyMax = 8192;
volatile std::uint64_t g_bind_probe_value = 0;

struct sockaddr_vm {
    ADDRESS_FAMILY svm_family;
    std::uint16_t svm_reserved1;
    std::uint32_t svm_port;
    std::uint32_t svm_cid;
    std::uint8_t svm_zero[4];
};

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

bool query_module_base_local(std::uint64_t pid, const char* module_name, std::uint64_t* out_base, std::string* error) {
    if (pid == 0 || module_name == nullptr || out_base == nullptr) {
        if (error) {
            *error = "invalid query_module_base_local args";
        }
        return false;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
        static_cast<DWORD>(pid)
    );
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = "CreateToolhelp32Snapshot failed";
        }
        return false;
    }

    MODULEENTRY32 me{};
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32First(snapshot, &me)) {
        do {
            std::string name(me.szModule ? me.szModule : "");
            for (char& ch : name) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            std::string target(module_name);
            for (char& ch : target) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            if (name == target) {
                *out_base = reinterpret_cast<std::uint64_t>(me.modBaseAddr);
                found = true;
                break;
            }
        } while (Module32Next(snapshot, &me));
    }
    CloseHandle(snapshot);

    if (!found) {
        if (error) {
            *error = std::string("module not found: ") + module_name;
        }
        return false;
    }
    return true;
}

bool extract_json_token(const std::string& text, const std::string& key, std::string* out) {
    if (out == nullptr) return false;
    const std::string qk = "\"" + key + "\"";
    const std::size_t key_pos = text.find(qk);
    if (key_pos == std::string::npos) return false;
    const std::size_t colon = text.find(':', key_pos + qk.size());
    if (colon == std::string::npos) return false;
    std::size_t pos = colon + 1;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    if (pos >= text.size()) return false;
    if (text[pos] == '"') {
        const std::size_t end = text.find('"', pos + 1);
        if (end == std::string::npos) return false;
        *out = text.substr(pos + 1, end - pos - 1);
        return true;
    }
    std::size_t end = pos;
    while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != '\r' && text[end] != '\n') {
        ++end;
    }
    *out = text.substr(pos, end - pos);
    return true;
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

VsockMemoryReader::~VsockMemoryReader() = default;

bool VsockMemoryReader::connect_vsock(std::uintptr_t* out_sock, std::string* error) {
    if (out_sock == nullptr) {
        if (error) {
            *error = "out_sock is null";
        }
        return false;
    }
    if (!global_wsa().ok()) {
        if (error) {
            *error = "WSAStartup failed";
        }
        return false;
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

    *out_sock = static_cast<std::uintptr_t>(sock);
    return true;
}

void VsockMemoryReader::close_socket(std::uintptr_t sock) {
    const SOCKET raw = static_cast<SOCKET>(sock);
    if (raw != INVALID_SOCKET) {
        closesocket(raw);
    }
}

bool VsockMemoryReader::send_text(std::uintptr_t sock, const std::string& text, std::string* error) {
    return send_all(static_cast<SOCKET>(sock), reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), error);
}

bool VsockMemoryReader::recv_text(std::uintptr_t sock, std::string* out_text, std::string* error) {
    if (out_text == nullptr) {
        if (error) {
            *error = "out_text is null";
        }
        return false;
    }

    out_text->clear();
    std::array<char, 1024> chunk{};
    while (out_text->size() < kHandshakeReplyMax) {
        const int rc = recv(static_cast<SOCKET>(sock), chunk.data(), static_cast<int>(chunk.size()), 0);
        if (rc == SOCKET_ERROR) {
            if (error) {
                *error = "recv failed: " + win_error_message(static_cast<DWORD>(WSAGetLastError()));
            }
            return false;
        }
        if (rc == 0) {
            break;
        }
        out_text->append(chunk.data(), static_cast<std::size_t>(rc));
        if (out_text->find('\n') != std::string::npos) {
            break;
        }
    }

    if (out_text->empty()) {
        if (error) {
            *error = "empty INIT_BIND reply";
        }
        return false;
    }
    return true;
}

bool VsockMemoryReader::init_bind_once(std::uint64_t target_pid, std::string* error) {
    if (target_pid == 0) {
        if (error) {
            *error = "target_pid is 0";
        }
        return false;
    }

    std::uintptr_t sock = static_cast<std::uintptr_t>(~std::uintptr_t{0});
    if (!connect_vsock(&sock, error)) {
        return false;
    }

    std::uint64_t target_chain_root_va = 0;
    {
        std::uint64_t module_base = 0;
        std::string modErr;
        if (query_module_base_local(target_pid, "mhmain.dll", &module_base, &modErr)) {
            target_chain_root_va = module_base + GAME_PIT_CHAIN_BASE_OFFSET;
        } else {
            BOT_WARN("MemoryReader", "query_module_base_local 失败，Host 侧可能需自行解析模块: " << modErr);
        }
    }

    // INIT 自检探针：Bot 先写入本地值，要求 Host 反向读取并回传该值。
    const std::uint64_t probe_value =
        (static_cast<std::uint64_t>(GetTickCount64()) << 24U) ^
        static_cast<std::uint64_t>(target_pid) ^
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&g_shared_data)) ^
        0xA5A55A5AA55A5A5AULL;
    g_bind_probe_value = probe_value;

    std::ostringstream req;
    req << "{\"cmd\":\"INIT_BIND\",\"data\":{"
        << "\"bot_pid\":" << static_cast<std::uint64_t>(GetCurrentProcessId()) << ","
        << "\"bot_receive_addr\":\"0x" << std::hex
        << reinterpret_cast<std::uintptr_t>(&g_shared_data)
        << std::dec << "\","
        << "\"bot_probe_addr\":\"0x" << std::hex
        << reinterpret_cast<std::uintptr_t>(&g_bind_probe_value)
        << std::dec << "\","
        << "\"bot_probe_value\":\"0x" << std::hex
        << probe_value
        << std::dec << "\","
        << "\"target_game_pid\":" << target_pid << ","
        << "\"target_game_name\":\"mhmain.exe\","
        << "\"target_base_addr\":\"mhmain.dll+0x" << std::hex
        << GAME_PIT_CHAIN_BASE_OFFSET
        << std::dec << "\","
        << "\"target_chain_root_va\":\"0x" << std::hex
        << target_chain_root_va
        << std::dec << "\","
        << "\"target_offsets\":["
        << GAME_PIT_POS_STRUCT_OFFSET << ","
        << (GAME_PIT_POS_STRUCT_OFFSET + 4) << ","
        // 第 3 个偏移位预留 map_id；若目标结构无该字段，Host 可忽略或写 0。
        << (GAME_PIT_POS_STRUCT_OFFSET + 8)
        << "]"
        << "}}\n";

    std::string reply;
    const bool ok = send_text(sock, req.str(), error) && recv_text(sock, &reply, error);
    close_socket(sock);
    if (!ok) {
        return false;
    }

    const bool accepted =
        reply.find("HOST_READY_DATA_LINK_ESTABLISHED") != std::string::npos ||
        reply.find("\"status\":\"success\"") != std::string::npos ||
        reply.find("\"status\": \"success\"") != std::string::npos;

    if (!accepted) {
        if (error) {
            *error = "INIT_BIND rejected: " + reply;
        }
        return false;
    }

    std::string probe_read_text;
    if (!extract_json_token(reply, "probe_read_value", &probe_read_text)) {
        if (error) {
            *error = "INIT_BIND missing probe_read_value: " + reply;
        }
        return false;
    }
    std::uint64_t probe_read_value = 0;
    try {
        probe_read_value = std::stoull(probe_read_text, nullptr, 0);
    } catch (...) {
        if (error) {
            *error = "INIT_BIND invalid probe_read_value: " + probe_read_text;
        }
        return false;
    }
    if (probe_read_value != probe_value) {
        if (error) {
            std::ostringstream oss;
            oss << "INIT_BIND probe mismatch expected=0x" << std::hex << probe_value
                << " actual=0x" << probe_read_value << std::dec;
            *error = oss.str();
        }
        return false;
    }

    BOT_LOG("MemoryReader", "INIT_BIND 完成 target_pid=" << target_pid
            << " shared_addr=0x" << std::hex << reinterpret_cast<std::uintptr_t>(&g_shared_data)
            << std::dec);
    return true;
}

bool VsockMemoryReader::initialize_binding(std::uint64_t target_pid, std::string* error) {
    std::lock_guard<std::mutex> lk(bind_mu_);
    if (bind_done_) {
        if (error) {
            error->clear();
        }
        return true;
    }
    if (!init_bind_once(target_pid, error)) {
        return false;
    }
    bind_done_ = true;
    if (error) {
        error->clear();
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
    (void)pid;
    (void)va;
    (void)out;
    (void)size;
    if (error) {
        *error = "vsock direct read is disabled; use SharedDataStatus feed";
    }
    return false;
}

bool VsockMemoryReader::query_module_base_by_pid(
    std::uint64_t pid,
    const std::string& module_name,
    std::uint64_t* out_base,
    std::string* error
) {
    (void)module_name;
    if (out_base == nullptr) {
        if (error) {
            *error = "out_base is null";
        }
        return false;
    }
    if (!initialize_binding(pid, error)) {
        return false;
    }
    *out_base = 0;
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
