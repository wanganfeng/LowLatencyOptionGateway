#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int socket_t;
#endif

#include "protocol.h"

void send_packet(socket_t client, std::uint16_t type, const void* payload, std::size_t payload_len) {
    Header h;
    h.total_len = htonl(static_cast<uint32_t>(sizeof(Header) + payload_len));
    h.msg_type = htons(type);
    h.seq = 0;
    h.sender_id = 99;
    send(client, (const char*)&h, sizeof(Header), 0);
    send(client, (const char*)payload, (int)payload_len, 0);
}

int main() {
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    socket_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    addr.sin_addr.s_addr = INADDR_ANY;
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 3);
    std::cout << "[FAKE EXCHANGE] Listening on port 9000...\n";

    socket_t client = accept(server_fd, nullptr, nullptr);
    std::cout << "[FAKE EXCHANGE] Gateway connected!\n";

    // 1. 发 100 个期权行情包
    for (int i = 0; i < 100; ++i) {
        FillPayload p;
        p.order_id = 5000 + i;
        p.S = 3500.0 + i; p.K = 3600.0; p.T = 30.0 / 365.0;
        p.r = 0.03; p.sigma = 0.25; p.is_call = 1;
        send_packet(client, MSG_ORDER_FILL, &p, sizeof(p));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::cout << "[FAKE EXCHANGE] Market data sent. Processing orders...\n";

    // 2. 循环接收新单和撤单，但设置 2 秒的存活倒计时
    std::vector<char> recv_buf(4096);
    auto start_time = std::chrono::steady_clock::now();

    // 设置 Socket 接收超时，防止 recv 死等导致时间检查失效
#ifdef _WIN32
    DWORD timeout = 10; // 10ms
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 10000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif

    while (true) {
        // 如果超过 2 秒，强制退出
        if (std::chrono::steady_clock::now() - start_time > std::chrono::seconds(2)) {
            std::cout << "[FAKE EXCHANGE] 2 seconds is up. Shutting down...\n";
            break;
        }

        int n = recv(client, recv_buf.data(), (int)recv_buf.size(), 0);
        if (n <= 0) continue; // 超时或出错，继续循环检查时间

        std::size_t offset = 0;
        while (offset + sizeof(Header) <= (std::size_t)n) {
            Header h;
            std::memcpy(&h, recv_buf.data() + offset, sizeof(Header));
            std::uint32_t len = ntohl(h.total_len);
            std::uint16_t type = ntohs(h.msg_type);

            if (type == MSG_NEW_ORDER) {
                NewOrderPayload p;
                std::memcpy(&p, recv_buf.data() + offset + sizeof(Header), sizeof(p));
                AckPayload ack; ack.order_id = p.order_id;
                send_packet(client, MSG_ORDER_ACK, &ack, sizeof(ack));
                std::cout << "[FAKE EXCHANGE] Got NEW ORDER #" << p.order_id << ", sent ACK.\n";
            }
            else if (type == MSG_CANCEL_ORDER) {
                CancelPayload c;
                std::memcpy(&c, recv_buf.data() + offset + sizeof(Header), sizeof(c));
                std::cout << "[FAKE EXCHANGE] Got CANCEL #" << c.order_id << ", processing...\n";
                send_packet(client, MSG_ORDER_CANCELED, &c, sizeof(c));
            }
            offset += len;
        }
    }

#ifdef _WIN32
    closesocket(client);
    closesocket(server_fd);
    WSACleanup();
#else
    close(client);
    close(server_fd);
#endif
    return 0;
}