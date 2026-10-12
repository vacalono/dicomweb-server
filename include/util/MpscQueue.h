#pragma once
// 多生产者/单消费者无锁环形队列：用于解耦 I/O 线程与业务线程。
// 注意：容量建议取 2 的幂；head_ 由生产者竞争，tail_ 仅消费者修改。
#include <atomic>
#include <cstddef>
#include <optional>
#include <vector>

namespace dws {

template <typename T>
class MpscQueue {
 public:
  explicit MpscQueue(size_t capacity)
      : cap_(RoundUpPow2(capacity)), mask_(cap_ - 1), buf_(cap_) {}

  bool Push(T v) {
    size_t head = head_.load(std::memory_order_relaxed);
    for (;;) {
      size_t tail = tail_.load(std::memory_order_acquire);
      if (head - tail >= cap_) return false;  // 满：调用方决定丢弃或阻塞
      if (head_.compare_exchange_weak(head, head + 1, std::memory_order_relaxed)) {
        buf_[head & mask_].store(std::move(v), std::memory_order_release);
        return true;
      }
    }
  }

  std::optional<T> Pop() {
    size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return std::nullopt;  // 空
    T v = buf_[tail & mask_].load(std::memory_order_acquire);
    tail_.store(tail + 1, std::memory_order_release);
    return v;
  }

  size_t size() const {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }
  size_t capacity() const { return cap_; }

 private:
  static size_t RoundUpPow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
  }
  const size_t cap_;
  const size_t mask_;
  std::vector<std::atomic<T>> buf_;
  std::atomic<size_t> head_{0};  // 生产者
  std::atomic<size_t> tail_{0};  // 消费者
};

}  // namespace dws
