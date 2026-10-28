#pragma once
// 令牌桶限流：按全局 QPS 控制入口请求。桶容量 = 突发上限，匀速补充。
// 放在 Router::Dispatch 最前面，超限直接 429（不进入业务逻辑，保护下游）。
#include <algorithm>
#include <chrono>
#include <mutex>

namespace dws {

class TokenBucket {
 public:
  TokenBucket(double rps, double burst) : rps_(rps), burst_(burst), tokens_(burst) {}

  bool TryAcquire() {
    std::lock_guard<std::mutex> lock(mu_);
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - last_).count();
    last_ = now;
    tokens_ = std::min(burst_, tokens_ + dt * rps_);
    if (tokens_ < 1.0) return false;
    tokens_ -= 1.0;
    return true;
  }

  double rps() const { return rps_; }

 private:
  std::mutex mu_;
  double rps_;
  double burst_;
  double tokens_;
  std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

}  // namespace dws
