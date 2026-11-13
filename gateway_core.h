#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include "protocol.h"

enum class OrderState : std::uint8_t {
    New, PendingSubmit, Submitted, PartiallyFilled, Filled, Rejected, CancelPending, Cancelled
};

struct Order {
    std::uint64_t order_id = 0;
    double        price = 0.0;
    std::int32_t  quantity = 0;
    std::int32_t  filled_qty = 0;
    OrderState    state = OrderState::New;
    bool          is_active = false;
};

constexpr std::size_t HASH_CAPACITY = 1024 * 1024;

class OrderHashMap {
public:
    void insert(std::uint64_t id, const Order& order) {
        std::size_t idx = id & (HASH_CAPACITY - 1);
        while (true) {
            if (!table_[idx].is_active) {
                table_[idx] = order;
                table_[idx].is_active = true;
                return;
            }
            idx = (idx + 1) & (HASH_CAPACITY - 1);
        }
    }

    Order* find(std::uint64_t id) {
        std::size_t idx = id & (HASH_CAPACITY - 1);
        std::size_t start = idx;
        while (true) {
            if (!table_[idx].is_active) return nullptr;
            if (table_[idx].order_id == id) return &table_[idx];
            idx = (idx + 1) & (HASH_CAPACITY - 1);
            if (idx == start) return nullptr;
        }
    }

private:
    Order table_[HASH_CAPACITY];
};

// 发往发单线程的消息（包含新单和撤单）
struct OutboundMessage {
    Header header;
    union {
        NewOrderPayload new_order;
        CancelPayload  cancel;
    } data;
};

class OrderStateMachine {
public:
    static void on_ack(Order& order) {
        if (order.state == OrderState::PendingSubmit) {
            order.state = OrderState::Submitted;
        }
    }

    static void on_fill(Order& order, std::int32_t fill_qty) {
        if (order.state == OrderState::Submitted || order.state == OrderState::PartiallyFilled) {
            order.filled_qty += fill_qty;
            if (order.filled_qty >= order.quantity) {
                order.state = OrderState::Filled;
            }
            else {
                order.state = OrderState::PartiallyFilled;
            }
        }
    }

    static void on_cancel_sent(Order& order) {
        if (order.state == OrderState::Submitted || order.state == OrderState::PartiallyFilled) {
            order.state = OrderState::CancelPending;
        }
    }

    static void on_cancel_success(Order& order) {
        if (order.state == OrderState::CancelPending || order.state == OrderState::Submitted || order.state == OrderState::PartiallyFilled) {
            order.state = OrderState::Cancelled;
        }
    }

    static void on_reject(Order& order) {
        order.state = OrderState::Rejected;
    }
};