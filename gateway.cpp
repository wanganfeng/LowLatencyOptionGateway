#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <thread>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <intrin.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
typedef int socket_t;
#endif

#include "protocol.h"
#include "spsc_queue.h"
#include "latency_timer.h"
#include "gateway_core.h"
#include "bs_pricer.h"

constexpr std::size_t QUEUE_CAP = 1024 * 1024;
socket_t g_client_socket = INVALID_SOCKET;

constexpr int MAX_POSITIONS = 1024;
constexpr int NUM_WORKERS = 4;

// 绑核辅助宏
inline void bind_thread_to_core(int core_id) {
#ifdef _WIN32
    // 绑定到逻辑核心 0~15，注意这里是掩码
    SetThreadAffinityMask(GetCurrentThread(), 1ULL << core_id);
#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
}

struct alignas(32) PortfolioPool {
    double K[MAX_POSITIONS];
    double T[MAX_POSITIONS];
    double sigma[MAX_POSITIONS];
    double qty[MAX_POSITIONS];
    double is_call[MAX_POSITIONS];
    int count = 0;
};
PortfolioPool g_portfolio;

struct MarketDataCommand {
    std::uint64_t order_id;
    double S, K, T, r, sigma;
    bool is_call;
    std::int64_t enter_ts_us;
};

struct ReportMessage {
    std::uint16_t msg_type;
    union {
        AckPayload ack;
        FillPayload fill;
        RejectPayload reject;
        CancelPayload cancel;
    } data;
};

// ============================================================
// 并行计算任务与结果结构
// ============================================================
struct TaskChunk {
    double S;
    int start_idx;
    int end_idx;
};

struct TaskResult {
    double delta;
    double gamma;
    double vega;
};

SpscQueue<MarketDataCommand, QUEUE_CAP> g_inbound_queue;
SpscQueue<OutboundMessage, QUEUE_CAP>   g_outbound_queue;
SpscQueue<ReportMessage, QUEUE_CAP>     g_report_queue;

// 每个 Worker 有自己的任务队列
SpscQueue<TaskChunk, 1024> g_worker_tasks[NUM_WORKERS];
// 共享结果数组（因为只有一个 Writer (Worker) 和一个 Reader (主控)，所以用原子标志）
TaskResult g_worker_results[NUM_WORKERS];
std::atomic<bool> g_worker_done[NUM_WORKERS];

OrderHashMap g_order_map;
std::atomic<bool> g_running{ true };
SpscQueue<std::int64_t, QUEUE_CAP> g_bs_latency_queue;

bool send_all(socket_t fd, const char* buf, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        const int n = ::send(fd, buf + off, static_cast<int>(len - off), 0);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

void order_gateway_thread() {
    OutboundMessage out_msg;
    std::printf("[ORDER-GW] Thread started.\n");
    while (g_running.load(std::memory_order_relaxed)) {
        if (g_outbound_queue.try_pop(out_msg)) {
            if (out_msg.header.msg_type == MSG_NEW_ORDER) {
                std::printf("[ORDER-GW] -> SEND NEW ORDER #%llu\n", (unsigned long long)out_msg.data.new_order.order_id);
            }
            else if (out_msg.header.msg_type == MSG_CANCEL_ORDER) {
                std::printf("[ORDER-GW] -> SEND CANCEL ORDER #%llu\n", (unsigned long long)out_msg.data.cancel.order_id);
            }

            Header network_header = out_msg.header;
            network_header.total_len = htonl(network_header.total_len);
            network_header.msg_type = htons(network_header.msg_type);

            send_all(g_client_socket, (const char*)&network_header, sizeof(Header));
            if (out_msg.header.msg_type == MSG_NEW_ORDER) {
                send_all(g_client_socket, (const char*)&out_msg.data.new_order, sizeof(NewOrderPayload));
            }
            else if (out_msg.header.msg_type == MSG_CANCEL_ORDER) {
                send_all(g_client_socket, (const char*)&out_msg.data.cancel, sizeof(CancelPayload));
            }
        }
        else {
#ifdef _WIN32
            _mm_pause();
#else
            __builtin_ia32_pause();
#endif
        }
    }
}

void oms_thread() {
    ReportMessage rep;
    std::printf("[OMS] Thread started.\n");
    while (g_running.load(std::memory_order_relaxed)) {
        if (g_report_queue.try_pop(rep)) {
            std::uint64_t oid = 0;
            if (rep.msg_type == MSG_ORDER_ACK || rep.msg_type == MSG_ORDER_CANCELED) oid = rep.data.ack.order_id;
            else if (rep.msg_type == MSG_ORDER_REJECT) oid = rep.data.reject.order_id;

            Order* order = g_order_map.find(oid);
            if (!order) continue;

            if (rep.msg_type == MSG_ORDER_ACK) {
                OrderStateMachine::on_ack(*order);
                std::printf("[OMS] Order #%llu -> SUBMITTED\n", (unsigned long long)oid);
            }
            else if (rep.msg_type == MSG_ORDER_CANCELED) {
                OrderStateMachine::on_cancel_success(*order);
                std::printf("[OMS] Order #%llu -> CANCELLED\n", (unsigned long long)oid);
            }
        }
        else {
#ifdef _WIN32
            _mm_pause();
#else
            __builtin_ia32_pause();
#endif
        }
    }
}

// ============================================================
// Worker 线程：算自己的任务，写自己的标志
// ============================================================
void worker_thread(int worker_id) {
    // 绑定到物理核 2, 4, 6, 8 (避开主控的 0 和 1)
    bind_thread_to_core(worker_id * 2 + 2);

    TaskChunk task;
    while (g_running.load(std::memory_order_relaxed)) {
        if (g_worker_tasks[worker_id].try_pop(task)) {
            __m256d sum_d = _mm256_setzero_pd();
            __m256d sum_g = _mm256_setzero_pd();
            __m256d sum_v = _mm256_setzero_pd();
            __m256d S_vec = _mm256_set1_pd(task.S);
            __m256d r_vec = _mm256_set1_pd(0.03);
            __m256d qty_vec = _mm256_set1_pd(10.0);

            int i = task.start_idx;
            for (; i <= task.end_idx - 4; i += 4) {
                __m256d K_vec = _mm256_load_pd(&g_portfolio.K[i]);
                __m256d T_vec = _mm256_load_pd(&g_portfolio.T[i]);
                __m256d sig_vec = _mm256_load_pd(&g_portfolio.sigma[i]);
                __m256d call_vec = _mm256_load_pd(&g_portfolio.is_call[i]);

                __m256d out_d, out_g, out_v;
                BSPricer::calculate_greeks_avx(S_vec, K_vec, T_vec, r_vec, sig_vec, call_vec, out_d, out_g, out_v);

                sum_d = _mm256_add_pd(sum_d, _mm256_mul_pd(out_d, qty_vec));
                sum_g = _mm256_add_pd(sum_g, _mm256_mul_pd(out_g, qty_vec));
                sum_v = _mm256_add_pd(sum_v, _mm256_mul_pd(out_v, qty_vec));
            }

            // 尾部掩码处理
            int remainder = (task.end_idx - i) + 1;
            if (remainder > 0) {
                long long mask_arr[4] = { 0, 0, 0, 0 };
                for (int k = 0; k < remainder; k++) mask_arr[k] = -1LL;
                __m256i mask = _mm256_loadu_si256((__m256i*)mask_arr);

                __m256d out_d, out_g, out_v;
                BSPricer::calculate_greeks_avx_masked(S_vec, &g_portfolio.K[i], &g_portfolio.T[i], r_vec,
                    &g_portfolio.sigma[i], &g_portfolio.is_call[i], mask,
                    out_d, out_g, out_v);

                sum_d = _mm256_add_pd(sum_d, _mm256_mul_pd(out_d, qty_vec));
                sum_g = _mm256_add_pd(sum_g, _mm256_mul_pd(out_g, qty_vec));
                sum_v = _mm256_add_pd(sum_v, _mm256_mul_pd(out_v, qty_vec));
            }

            double d_arr[4], g_arr[4], v_arr[4];
            _mm256_storeu_pd(d_arr, sum_d);
            _mm256_storeu_pd(g_arr, sum_g);
            _mm256_storeu_pd(v_arr, sum_v);

            // 写入全局结果，并设置完成标志 (Release 保证结果写入先于 flag)
            g_worker_results[worker_id].delta = d_arr[0] + d_arr[1] + d_arr[2] + d_arr[3];
            g_worker_results[worker_id].gamma = g_arr[0] + g_arr[1] + g_arr[2] + g_arr[3];
            g_worker_results[worker_id].vega = v_arr[0] + v_arr[1] + v_arr[2] + v_arr[3];
            g_worker_done[worker_id].store(true, std::memory_order_release);
        }
        else {
#ifdef _WIN32
            _mm_pause();
#else
            __builtin_ia32_pause();
#endif
        }
    }
}

// ============================================================
// 策略线程：主控
// ============================================================
void strategy_thread() {
    // 主控绑到物理核 0
    bind_thread_to_core(0);

    MarketDataCommand cmd;
    std::uint64_t internal_order_id = 100000;

    struct PendingCancel { std::uint64_t id; std::int64_t ts_us; };
    PendingCancel pending_cancels[1000];
    int pending_count = 0;

    std::printf("[STRATEGY] Master Thread started with Parallel AVX2.\n");

    while (g_running.load(std::memory_order_relaxed)) {
        if (g_inbound_queue.try_pop(cmd)) {
            std::int64_t calc_start = LatencyTimer::now_us();

            // 1. 记录新持仓
            if (g_portfolio.count < MAX_POSITIONS) {
                int idx = g_portfolio.count;
                g_portfolio.K[idx] = cmd.K;
                g_portfolio.T[idx] = cmd.T;
                g_portfolio.sigma[idx] = cmd.sigma;
                g_portfolio.qty[idx] = 10.0;
                g_portfolio.is_call[idx] = cmd.is_call ? 1.0 : 0.0;
                g_portfolio.count++;
            }

            // 2. 切分任务，并清零完成标志
            int total = g_portfolio.count;
            int chunk_size = total / NUM_WORKERS;
            int rem = total % NUM_WORKERS;

            int start = 0;
            for (int w = 0; w < NUM_WORKERS; ++w) {
                int end = start + chunk_size + (w < rem ? 1 : 0) - 1;
                if (start <= end) {
                    g_worker_done[w].store(false, std::memory_order_relaxed);
                    g_worker_tasks[w].try_push({ cmd.S, start, end });
                }
                else {
                    g_worker_done[w].store(true, std::memory_order_relaxed); // 没任务直接完成
                }
                start = end + 1;
            }

            // 3. 【关键】等待 4 个 Worker 全部完成 (带自旋保护)
            for (int w = 0; w < NUM_WORKERS; ++w) {
                // 如果 100 次自旋还没完成，说明 Worker 被系统抢占，先放弃
                int spins = 0;
                while (!g_worker_done[w].load(std::memory_order_acquire)) {
#ifdef _WIN32
                    _mm_pause();
#else
                    __builtin_ia32_pause();
#endif
                    if (++spins > 1000) break;
                }
            }

            // 4. 汇总结果
            double total_delta = 0.0, total_gamma = 0.0, total_vega = 0.0;
            for (int w = 0; w < NUM_WORKERS; ++w) {
                if (g_worker_done[w].load(std::memory_order_acquire)) {
                    total_delta += g_worker_results[w].delta;
                    total_gamma += g_worker_results[w].gamma;
                    total_vega += g_worker_results[w].vega;
                }
            }

            std::int64_t calc_end = LatencyTimer::now_us();
            g_bs_latency_queue.try_push(calc_end - calc_start);

            if (cmd.order_id % 10 == 0) {
                std::printf("[PORTFOLIO] S=%.2f | Pos=%d | Delta=%.2f | Gamma=%.4f | Vega=%.2f\n",
                    cmd.S, g_portfolio.count, total_delta, total_gamma, total_vega);
            }

            // 交易闭环
            double price = BSPricer::calculate_price(cmd.S, cmd.K, cmd.T, cmd.r, cmd.sigma, cmd.is_call);
            if (price > 100.0 && pending_count < 1000) {
                OutboundMessage out_msg{};
                out_msg.header.total_len = sizeof(Header) + sizeof(NewOrderPayload);
                out_msg.header.msg_type = MSG_NEW_ORDER;

                std::uint64_t new_id = internal_order_id++;
                out_msg.data.new_order.order_id = new_id;
                out_msg.data.new_order.price = cmd.S + 0.5;
                out_msg.data.new_order.qty = 10;
                out_msg.data.new_order.side = 0;

                Order new_order;
                new_order.order_id = new_id;
                new_order.quantity = 10;
                new_order.state = OrderState::PendingSubmit;
                g_order_map.insert(new_id, new_order);

                g_outbound_queue.try_push(out_msg);
                pending_cancels[pending_count++] = { new_id, LatencyTimer::now_us() };
            }
        }

        // 超时撤单
        std::int64_t now = LatencyTimer::now_us();
        for (int j = 0; j < pending_count; ) {
            if (now - pending_cancels[j].ts_us > 100000) {
                Order* o = g_order_map.find(pending_cancels[j].id);
                if (o && o->state == OrderState::Submitted) {
                    OutboundMessage cancel_msg{};
                    cancel_msg.header.total_len = sizeof(Header) + sizeof(CancelPayload);
                    cancel_msg.header.msg_type = MSG_CANCEL_ORDER;
                    cancel_msg.data.cancel.order_id = pending_cancels[j].id;

                    g_outbound_queue.try_push(cancel_msg);
                    OrderStateMachine::on_cancel_sent(*o);
                }
                pending_cancels[j] = pending_cancels[--pending_count];
            }
            else {
                j++;
            }
        }
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    WSADATA wsaData; WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    for (int i = 0; i < NUM_WORKERS; ++i) {
        g_worker_done[i].store(false);
    }

    // 启动 4 个 Worker，绑定到核心 2~8 (避免和主控、网络线程抢资源)
    std::thread workers[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; ++i) {
        workers[i] = std::thread(worker_thread, i);
    }

    std::thread t_gw(order_gateway_thread);
    std::thread t_oms(oms_thread);
    std::thread t_strategy(strategy_thread);

    // 主 IO 线程绑定到核心 0
    bind_thread_to_core(0);

    g_client_socket = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int retry = 0;
    while (::connect(g_client_socket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR && retry < 10) {
        std::printf("[GATEWAY] Waiting for server... (%d)\n", ++retry);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (retry >= 10) return 1;
    std::printf("[GATEWAY] Connected.\n");

    int flag = 1;
    setsockopt(g_client_socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));

    std::vector<char> recv_buffer(64 * 1024);
    std::vector<char> pending_buffer(256 * 1024);
    std::size_t pending_len = 0;
    PacketParser parser;

    while (true) {
        int n = ::recv(g_client_socket, recv_buffer.data(), (int)recv_buffer.size(), 0);
        if (n <= 0) break;

        if (pending_len + n > pending_buffer.size())
            pending_buffer.resize(pending_len + n + 1024 * 1024);

        std::memcpy(pending_buffer.data() + pending_len, recv_buffer.data(), n);
        pending_len += n;

        std::size_t offset = 0;
        while (offset < pending_len) {
            std::span<const std::byte> buf_span(reinterpret_cast<const std::byte*>(pending_buffer.data() + offset), pending_len - offset);
            auto pkt = parser.parse(buf_span);
            if (!pkt) break;

            if (pkt->msg_type == MSG_ORDER_FILL) {
                FillPayload fill;
                std::memcpy(&fill, pkt->payload.data(), sizeof(FillPayload));

                if (fill.order_id < 100000) {
                    MarketDataCommand cmd;
                    cmd.order_id = fill.order_id;
                    cmd.S = fill.S;
                    cmd.K = fill.K;
                    cmd.T = fill.T;
                    cmd.r = fill.r;
                    cmd.sigma = fill.sigma;
                    cmd.is_call = fill.is_call != 0;
                    g_inbound_queue.try_push(cmd);
                }
            }
            else if (pkt->msg_type == MSG_ORDER_ACK) {
                AckPayload ack;
                std::memcpy(&ack, pkt->payload.data(), sizeof(AckPayload));
                ReportMessage rep; rep.msg_type = MSG_ORDER_ACK; rep.data.ack = ack;
                g_report_queue.try_push(rep);
            }
            else if (pkt->msg_type == MSG_ORDER_CANCELED) {
                CancelPayload cxl;
                std::memcpy(&cxl, pkt->payload.data(), sizeof(CancelPayload));
                ReportMessage rep; rep.msg_type = MSG_ORDER_CANCELED; rep.data.cancel = cxl;
                g_report_queue.try_push(rep);
            }
            offset += (pkt->header.size() + pkt->payload.size());
        }

        if (offset > 0 && offset < pending_len)
            std::memmove(pending_buffer.data(), pending_buffer.data() + offset, pending_len - offset);
        pending_len -= offset;
    }

#ifdef _WIN32
    closesocket(g_client_socket); WSACleanup();
#endif
    g_running = false;

    for (int i = 0; i < NUM_WORKERS; ++i) workers[i].join();
    t_strategy.join(); t_oms.join(); t_gw.join();

    std::vector<std::int64_t> latencies;
    std::int64_t l;
    while (g_bs_latency_queue.try_pop(l)) latencies.push_back(l);

    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        std::int64_t sum = std::accumulate(latencies.begin(), latencies.end(), 0LL);
        std::printf("\n========== [Parallel Masked AVX2 Greeks Latency] ==========\n");
        std::printf("Samples: %zu\n", latencies.size());
        std::printf("Avg: %lld us\n", (long long)(sum / latencies.size()));
        std::printf("Min: %lld us\n", (long long)latencies.front());
        std::printf("P99: %lld us\n", (long long)latencies[latencies.size() * 0.99]);
        std::printf("Max: %lld us\n", (long long)latencies.back());
        std::printf("===========================================================\n");
    }
    return 0;
}