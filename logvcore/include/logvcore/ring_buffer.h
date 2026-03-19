#pragma once
#include <atomic>
#include <array>
#include <cstddef>
#include <type_traits>

namespace logvcore {

/// Lock-free Single-Producer Single-Consumer ring buffer.
/// N must be a power of two. Uses acquire/release semantics on two atomics —
/// no mutex, no futex, no false sharing between head and tail (padding).
///
///  Producer thread calls push()  (SSH reader)
///  Consumer thread calls pop() / pop_many()  (Qt timer, main thread)
template<typename T, std::size_t N>
class SPSCRingBuffer {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of 2");
    static_assert(std::is_move_assignable_v<T>);

    static constexpr std::size_t MASK = N - 1;

    // Pad to separate cache lines so producer and consumer don't fight.
    alignas(64) std::atomic<std::size_t> head_{0};  // written by producer
    alignas(64) std::atomic<std::size_t> tail_{0};  // written by consumer
    alignas(64) std::array<T, N> buf_{};

public:
    /// Try to push one item. Returns false if buffer is full (drop it or back-off).
    bool push(T item) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t next_h = (h + 1) & MASK;
        if (next_h == tail_.load(std::memory_order_acquire))
            return false;  // full
        buf_[h] = std::move(item);
        head_.store(next_h, std::memory_order_release);
        return true;
    }

    /// Try to pop one item. Returns false if buffer is empty.
    bool pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire))
            return false;  // empty
        out = std::move(buf_[t]);
        tail_.store((t + 1) & MASK, std::memory_order_release);
        return true;
    }

    /// Drain up to `max` items into `out` vector. Returns count drained.
    /// Amortises the atomic load cost — the inner loop is a tight copy.
    template<typename Vec>
    std::size_t pop_many(Vec& out, std::size_t max) {
        const std::size_t h = head_.load(std::memory_order_acquire);
        std::size_t t = tail_.load(std::memory_order_relaxed);
        std::size_t count = 0;
        while (count < max && t != h) {
            out.push_back(std::move(buf_[t]));
            t = (t + 1) & MASK;
            ++count;
        }
        if (count)
            tail_.store(t, std::memory_order_release);
        return count;
    }

    std::size_t size_approx() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & MASK;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    /// Reset head and tail to zero, discarding all contents.
    /// Only safe when no concurrent push/pop is in progress.
    void clear() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    static constexpr std::size_t capacity() noexcept { return N - 1; }
};

} // namespace logvcore
