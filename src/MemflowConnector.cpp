#include "MemflowConnector.h"
#include "BotLogger.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <sstream>

// vsock on windows usually uses these defines if not in headers
#ifndef AF_VSOCK
#define AF_VSOCK 40
#endif

// For Windows Guest, CID is usually 2 (Host)
#define VMADDR_CID_HOST 2
#define MEMFLOW_BACKEND_PORT 4050

#pragma comment(lib, "Ws2_32.lib")

// Simple VSOCK address structure for Windows
struct sockaddr_vm {
    unsigned short svm_family;
    unsigned short svm_reserved1;
    unsigned int svm_port;
    unsigned int svm_cid;
    unsigned char svm_zero[4];
};

MemflowConnector::MemflowConnector() : stopFlag(false) {
    buffer.sync_flag = 0;
}

MemflowConnector::~MemflowConnector() {
    stop();
}

bool MemflowConnector::start(uint32_t targetPid, uint64_t dllBase) {
    stopFlag = false;
    workerThread = std::thread(&MemflowConnector::threadMain, this, targetPid, dllBase);
    return true;
}

void MemflowConnector::stop() {
    stopFlag = true;
    if (workerThread.joinable()) {
        workerThread.join();
    }
}

void MemflowConnector::threadMain(uint32_t targetPid, uint64_t dllBase) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        BOT_ERR("Memflow", "WSAStartup failed");
        return;
    }

    while (!stopFlag) {
        SOCKET s = socket(AF_VSOCK, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) {
            BOT_ERR("Memflow", "Socket creation failed, err=" << WSAGetLastError());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        sockaddr_vm addr{};
        addr.svm_family = AF_VSOCK;
        addr.svm_cid = VMADDR_CID_HOST;
        addr.svm_port = MEMFLOW_BACKEND_PORT;

        BOT_LOG("Memflow", "Connecting to Host Backend (CID=" << addr.svm_cid << " Port=" << addr.svm_port << ")...");

        if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(s);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        BOT_LOG("Memflow", "Connected to Host. Sending INIT_BIND...");

        // Build handshake JSON
        std::ostringstream oss;
        oss << "{\"cmd\":\"INIT_BIND\", \"bot_pid\":" << GetCurrentProcessId() 
            << ", \"bot_receive_addr\":" << (uintptr_t)&buffer 
            << ", \"target_game_pid\":" << targetPid
            << ", \"target_base_addr\":\"" << std::hex << "0x" << dllBase << std::dec << "\""
            << ", \"target_offsets\":[0x118, 0x11c]"
            << ", \"role_offsets\":[0x208, 0x20c]"
            << "}\n";
        
        std::string handshake = oss.str();
        send(s, handshake.c_str(), (int)handshake.length(), 0);

        BOT_LOG("Memflow", "Handshake sent. Waiting for data...");

        // The host will now write directly to our memory.
        // We just keep the socket open to maintain the session.
        // We can also listen for any control messages from host if needed.
        char dummy[256];
        while (!stopFlag) {
            int n = recv(s, dummy, sizeof(dummy), 0);
            if (n <= 0) {
                BOT_WARN("Memflow", "Connection lost, retrying...");
                break;
            }
            // Optional: parse messages like "SHUTDOWN" etc.
        }

        closesocket(s);
        buffer.sync_flag = 0;
        if (!stopFlag) std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    WSACleanup();
}
