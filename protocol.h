#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>

// ============================================================
// 网络协议定义 (变长协议)
// ============================================================

// 消息类型
constexpr std::uint16_t MSG_NEW_ORDER = 101; // 策略发起新单
constexpr std::uint16_t MSG_ORDER_ACK = 102; // 交易所确认新单
constexpr std::uint16_t MSG_ORDER_FILL = 103; // 行情推流（这里专用于期权行情）
constexpr std::uint16_t MSG_ORDER_REJECT = 104; // 交易所拒单
constexpr std::uint16_t MSG_CANCEL_ORDER = 105; // 策略发起撤单
constexpr std::uint16_t MSG_ORDER_CANCELED = 106; // 交易所确认撤单

#pragma pack(push, 1)
// 通用包头 (12 字节)
struct Header {
    std::uint32_t total_len; // 整个包的长度 (包含 Header 本身)
    std::uint16_t msg_type;  // 消息类型
    std::uint16_t seq;       // 序列号
    std::uint32_t sender_id; // 发送方ID
};

struct NewOrderPayload {
    std::uint64_t order_id;
    double        price;
    std::int32_t  qty;
    std::uint8_t  side;
};

struct CancelPayload {
    std::uint64_t order_id;
};

struct AckPayload {
    std::uint64_t order_id;
};

// 【核心修改】期权行情 Payload，带上 BS 定价所需的所有参数
struct FillPayload {
    std::uint64_t order_id;    // 行情ID
    double        S;           // 标的资产现价 (Spot)
    double        K;           // 行权价 (Strike)
    double        T;           // 到期时间 (年化，如 30/365)
    double        r;           // 无风险利率
    double        sigma;       // 波动率
    std::int32_t  is_call;     // 1 为看涨，0 为看跌
};

struct RejectPayload {
    std::uint64_t order_id;
    std::int32_t  reject_code;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 12, "Header size mismatch");
static_assert(sizeof(NewOrderPayload) == 21, "NewOrderPayload size mismatch");
static_assert(sizeof(CancelPayload) == 8, "CancelPayload size mismatch");
static_assert(sizeof(AckPayload) == 8, "AckPayload size mismatch");
// FillPayload: 8 + 8 + 8 + 8 + 8 + 8 + 4 = 52
static_assert(sizeof(FillPayload) == 52, "FillPayload size mismatch");
static_assert(sizeof(RejectPayload) == 12, "RejectPayload size mismatch");

// ---------- 字节序工具 ----------
#if defined(_MSC_VER)
#include <stdlib.h>
inline std::uint64_t bswap64(std::uint64_t v) { return _byteswap_uint64(v); }
inline std::uint32_t bswap32(std::uint32_t v) { return _byteswap_ulong(v); }
#else
inline std::uint64_t bswap64(std::uint64_t v) { return __builtin_bswap64(v); }
inline std::uint32_t bswap32(std::uint32_t v) { return __builtin_bswap32(v); }
#endif

// ============================================================
// 变长包解析器
// ============================================================
struct RawPacket {
    std::span<const std::byte> header;
    std::span<const std::byte> payload;
    std::uint16_t msg_type = 0;
    std::uint16_t seq = 0;
};

class PacketParser {
public:
    static constexpr std::size_t HEADER_SIZE = sizeof(Header);

    std::optional<RawPacket> parse(std::span<const std::byte> buf) const {
        if (buf.size() < HEADER_SIZE) return std::nullopt;

        std::uint32_t total_len_be;
        std::memcpy(&total_len_be, buf.data(), 4);
        const std::uint32_t total_len = bswap32(total_len_be);

        if (total_len < HEADER_SIZE || buf.size() < total_len)
            return std::nullopt;

        RawPacket pkt;
        pkt.header = buf.subspan(0, HEADER_SIZE);
        pkt.payload = buf.subspan(HEADER_SIZE, total_len - HEADER_SIZE);

        std::uint16_t mt_be, seq_be;
        std::memcpy(&mt_be, buf.data() + 4, 2);
        std::memcpy(&seq_be, buf.data() + 6, 2);

        pkt.msg_type = static_cast<std::uint16_t>((mt_be >> 8) | (mt_be << 8));
        pkt.seq = static_cast<std::uint16_t>((seq_be >> 8) | (seq_be << 8));
        return pkt;
    }
};