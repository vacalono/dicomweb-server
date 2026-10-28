#pragma once
// 熔断器：连续失败达到阈值 → 打开（快速失败），冷却后进入半开放一个探针，
// 探针成功 → 关闭；失败 → 重新打开。用于保护下游（MySQL / Redis）。
#include <chrono>
#include <mutex>

namespace dws {

class CircuitBreaker {
 public:
  enum class State { kClosed, kOpen, kHalfOpen };

  CircuitBreaker(int failure_threshold, int open_ms)
      : threshold_(failure_threshold), open_ms_(open_ms) {}

  // 是否放行：Closed / HalfOpen（仅一个探针）放行；Open 且冷却未到 → 拒绝
  bool Allow() {
    std::lock_guard<std::mutex> lock(mu_);
    const auto now = std::chrono::steady_clock::now();
    if (state_ == State::kOpen) {
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - opened_at_).count();
      if (elapsed < open_ms_) return false;
      state_ = State::kHalfOpen;   // 冷却结束：允许一个探针
      probe_inflight_ = false;
    }
    if (state_ == State::kHalfOpen) {
      if (probe_inflight_) return false;
      probe_inflight_ = true;
    }
    return true;
  }

  void OnSuccess() {
    std::lock_guard<std::mutex> lock(mu_);
    failures_ = 0;
    state_ = State::kClosed;
    probe_inflight_ = false;
  }

  void OnFailure() {
    std::lock_guard<std::mutex> lock(mu_);
    probe_inflight_ = false;
    if (state_ == State::kHalfOpen) {   // 探针失败：立刻重新打开
      state_ = State::kOpen;
      opened_at_ = std::chrono::steady_clock::now();
      return;
    }
    if (++failures_ >= threshold_) {
      state_ = State::kOpen;
      opened_at_ = std::chrono::steady_clock::now();
    }
  }

  State state() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_;
  }

  const char* state_name() const {
    switch (state()) {
      case State::kClosed: return "closed";
      case State::kOpen: return "open";
      case State::kHalfOpen: return "half_open";
    }
    return "unknown";
  }

  int failures() const {
    std::lock_guard<std::mutex> lock(mu_);
    return failures_;
  }

 private:
  mutable std::mutex mu_;
  int threshold_;
  int open_ms_;
  int failures_ = 0;
  State state_ = State::kClosed;
  std::chrono::steady_clock::time_point opened_at_{};
  bool probe_inflight_ = false;
};

}  // namespace dws
