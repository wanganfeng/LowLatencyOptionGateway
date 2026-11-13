#pragma once
#include <atomic>
#include <cstddef>

template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity > 0, "Capacity must be > 0");
    static_assert((Capacity& (Capacity - 1)) == 0,
        "Capacity must be a power of two");

public:
    // 生产者线程调用
    bool try_push(const T& item) {
        const std::size_t w = head_.load(std::memory_order_relaxed);
        const std::size_t r = tail_.load(std::memory_order_acquire);
        if (w - r == Capacity) return false;

        items_[w & (Capacity - 1)] = item;
        head_.store(w + 1, std::memory_order_release);
        return true;
    }

    // 消费者线程调用
    bool try_pop(T& item) {
        const std::size_t r = tail_.load(std::memory_order_relaxed);
        const std::size_t w = head_.load(std::memory_order_acquire);
        if (r == w) return false;

        item = items_[r & (Capacity - 1)];
        tail_.store(r + 1, std::memory_order_release);
        return true;
    }

    // 仅调试用，非严格线程安全
    std::size_t size_approx() const {
        const std::size_t w = head_.load(std::memory_order_acquire);
        const std::size_t r = tail_.load(std::memory_order_acquire);
        return w - r;
    }

private:
    alignas(64) std::atomic<std::size_t> head_{ 0 };
    alignas(64) T items_[Capacity]{};
    alignas(64) std::atomic<std::size_t> tail_{ 0 };
};
