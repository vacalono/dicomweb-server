#pragma once
// 后台健康探测：每 interval_ms PING 一次下游（DAO / 缓存），驱动熔断器。
// 读取侧只看熔断状态（开 → 快速失败 503），不占用半开探针名额。
#include "util/CircuitBreaker.h"

#include <atomic>
#include <thread>

namespace dws {

class IMetadataDao;
class QidoCache;

class HealthGuard {
 public:
  HealthGuard(IMetadataDao* dao, QidoCache* cache, int interval_ms = 500);
  ~HealthGuard();

  void Start();
  void Stop();

  // 下游是否可用（熔断关闭或半开）
  bool db_up() const { return db_breaker_.state() != CircuitBreaker::State::kOpen; }
  bool cache_up() const { return cache_breaker_.state() != CircuitBreaker::State::kOpen; }

  CircuitBreaker& db_breaker() { return db_breaker_; }
  CircuitBreaker& cache_breaker() { return cache_breaker_; }

 private:
  void Loop();

  IMetadataDao* dao_ = nullptr;
  QidoCache* cache_ = nullptr;
  int interval_ms_ = 500;
  CircuitBreaker db_breaker_{3, 2000};      // 连续 3 次探测失败 → 打开 2 s
  CircuitBreaker cache_breaker_{3, 2000};
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace dws
