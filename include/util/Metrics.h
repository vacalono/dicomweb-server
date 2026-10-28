#pragma once
// 轻量指标（Prometheus 文本格式）：请求数/错误数/限流数/缓存命中/延迟直方图。
// 全部原子计数，读取无锁；不引入 prometheus-cpp 依赖。
#include <array>
#include <atomic>
#include <cstdint>
#include <sstream>
#include <string>

namespace dws {

class Metrics {
 public:
  static constexpr size_t kBuckets = 9;  // 1/2/5/10/25/50/100/250 ms, +Inf

  void IncRequests() { requests_.fetch_add(1, std::memory_order_relaxed); }
  void IncErrors() { errors_.fetch_add(1, std::memory_order_relaxed); }
  void IncRateLimited() { rate_limited_.fetch_add(1, std::memory_order_relaxed); }
  void IncCacheHit() { cache_hits_.fetch_add(1, std::memory_order_relaxed); }
  void IncCacheMiss() { cache_misses_.fetch_add(1, std::memory_order_relaxed); }

  void ObserveUs(uint64_t us) {
    latency_sum_us_.fetch_add(us, std::memory_order_relaxed);
    latency_count_.fetch_add(1, std::memory_order_relaxed);
    size_t i = 0;
    while (i + 1 < kBuckets && us > kBoundsUs[i]) ++i;
    latency_buckets_[i].fetch_add(1, std::memory_order_relaxed);
  }

  uint64_t requests() const { return requests_.load(std::memory_order_relaxed); }
  uint64_t errors() const { return errors_.load(std::memory_order_relaxed); }
  uint64_t rate_limited() const { return rate_limited_.load(std::memory_order_relaxed); }
  uint64_t cache_hits() const { return cache_hits_.load(std::memory_order_relaxed); }
  uint64_t cache_misses() const { return cache_misses_.load(std::memory_order_relaxed); }

  // Prometheus exposition（text/plain; version=0.0.4）
  std::string RenderPrometheus() const {
    std::ostringstream os;
    os << "# HELP dicomweb_requests_total HTTP requests handled\n"
       << "# TYPE dicomweb_requests_total counter\n"
       << "dicomweb_requests_total " << requests() << "\n"
       << "# HELP dicomweb_errors_total responses with status >= 500\n"
       << "# TYPE dicomweb_errors_total counter\n"
       << "dicomweb_errors_total " << errors() << "\n"
       << "# HELP dicomweb_rate_limited_total requests rejected by rate limiter\n"
       << "# TYPE dicomweb_rate_limited_total counter\n"
       << "dicomweb_rate_limited_total " << rate_limited() << "\n"
       << "# HELP dicomweb_qido_cache_hits_total QIDO cache hits\n"
       << "# TYPE dicomweb_qido_cache_hits_total counter\n"
       << "dicomweb_qido_cache_hits_total " << cache_hits() << "\n"
       << "# HELP dicomweb_qido_cache_misses_total QIDO cache misses\n"
       << "# TYPE dicomweb_qido_cache_misses_total counter\n"
       << "dicomweb_qido_cache_misses_total " << cache_misses() << "\n"
       << "# HELP dicomweb_request_duration_us request latency histogram (microseconds)\n"
       << "# TYPE dicomweb_request_duration_us histogram\n";
    uint64_t cumulative = 0;
    for (size_t i = 0; i < kBuckets; ++i) {
      cumulative += latency_buckets_[i].load(std::memory_order_relaxed);
      if (i + 1 < kBuckets) {
        os << "dicomweb_request_duration_us_bucket{le=\"" << kBoundsUs[i] << "\"} " << cumulative
           << "\n";
      } else {
        os << "dicomweb_request_duration_us_bucket{le=\"+Inf\"} " << cumulative << "\n";
      }
    }
    os << "dicomweb_request_duration_us_sum " << latency_sum_us_.load(std::memory_order_relaxed)
       << "\n"
       << "dicomweb_request_duration_us_count " << latency_count_.load(std::memory_order_relaxed)
       << "\n";
    return os.str();
  }

 private:
  static constexpr uint64_t kBoundsUs[kBuckets - 1] = {1000, 2000, 5000, 10000,
                                                       25000, 50000, 100000, 250000};
  std::atomic<uint64_t> requests_{0};
  std::atomic<uint64_t> errors_{0};
  std::atomic<uint64_t> rate_limited_{0};
  std::atomic<uint64_t> cache_hits_{0};
  std::atomic<uint64_t> cache_misses_{0};
  std::atomic<uint64_t> latency_sum_us_{0};
  std::atomic<uint64_t> latency_count_{0};
  std::array<std::atomic<uint64_t>, kBuckets> latency_buckets_{};
};

}  // namespace dws
