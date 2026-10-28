// 限流与指标的单测（不依赖网络/redis/mysql）
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "util/Metrics.h"
#include "util/RateLimiter.h"

TEST(TokenBucket, BurstThenRefill) {
  dws::TokenBucket bucket(/*rps=*/10.0, /*burst=*/1.0);
  EXPECT_TRUE(bucket.TryAcquire());   // 桶里就 1 个令牌
  EXPECT_FALSE(bucket.TryAcquire());  // 立刻再要：没有
  std::this_thread::sleep_for(std::chrono::milliseconds(150));  // 10 rps → 150ms 约 1.5 个
  EXPECT_TRUE(bucket.TryAcquire());
}

TEST(TokenBucket, RateIsRespectedOverTime) {
  dws::TokenBucket bucket(/*rps=*/50.0, /*burst=*/5.0);
  int allowed = 0;
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200)) {
    if (bucket.TryAcquire()) ++allowed;
  }
  // 200ms × 50 rps ≈ 10 个（加突发 5 个），允许较宽的范围
  EXPECT_GE(allowed, 6);
  EXPECT_LE(allowed, 25);
}

TEST(Metrics, CountersAndHistogramRender) {
  dws::Metrics m;
  m.IncRequests();
  m.IncRequests();
  m.IncErrors();
  m.IncRateLimited();
  m.IncCacheHit();
  m.IncCacheMiss();
  m.ObserveUs(500);     // ≤1ms 桶
  m.ObserveUs(30000);   // ≤50ms 桶

  const std::string text = m.RenderPrometheus();
  EXPECT_NE(text.find("dicomweb_requests_total 2"), std::string::npos);
  EXPECT_NE(text.find("dicomweb_errors_total 1"), std::string::npos);
  EXPECT_NE(text.find("dicomweb_rate_limited_total 1"), std::string::npos);
  EXPECT_NE(text.find("dicomweb_qido_cache_hits_total 1"), std::string::npos);
  EXPECT_NE(text.find("dicomweb_qido_cache_misses_total 1"), std::string::npos);
  EXPECT_NE(text.find("dicomweb_request_duration_us_count 2"), std::string::npos);
  EXPECT_NE(text.find("le=\"+Inf\"} 2"), std::string::npos);
  EXPECT_NE(text.find("le=\"1000\"} 1"), std::string::npos);   // 500us 落在 ≤1ms
}

TEST(Metrics, BucketsAreCumulative) {
  dws::Metrics m;
  m.ObserveUs(1500);
  m.ObserveUs(40000);
  const std::string text = m.RenderPrometheus();
  // ≤2ms 桶应包含 1500us 那一条；≤50ms 桶应累计两条
  EXPECT_NE(text.find("le=\"2000\"} 1"), std::string::npos);
  EXPECT_NE(text.find("le=\"50000\"} 2"), std::string::npos);
}
