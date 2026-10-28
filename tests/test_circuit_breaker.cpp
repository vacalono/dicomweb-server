// 熔断器状态机单测（不依赖网络）
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "util/CircuitBreaker.h"

namespace {
void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
}  // namespace

TEST(CircuitBreaker, OpensAfterThreshold) {
  dws::CircuitBreaker cb(/*threshold=*/3, /*open_ms=*/100);
  EXPECT_TRUE(cb.Allow());
  cb.OnFailure();
  cb.OnFailure();
  EXPECT_EQ(cb.state(), dws::CircuitBreaker::State::kClosed);
  cb.OnFailure();   // 第 3 次 → 打开
  EXPECT_EQ(cb.state(), dws::CircuitBreaker::State::kOpen);
  EXPECT_FALSE(cb.Allow());
}

TEST(CircuitBreaker, SuccessResetsFailureCount) {
  dws::CircuitBreaker cb(3, 100);
  cb.OnFailure();
  cb.OnFailure();
  cb.OnSuccess();   // 计数清零
  cb.OnFailure();
  cb.OnFailure();
  EXPECT_EQ(cb.state(), dws::CircuitBreaker::State::kClosed);
}

TEST(CircuitBreaker, HalfOpenAfterCooldownThenClosesOnSuccess) {
  dws::CircuitBreaker cb(2, 80);
  cb.OnFailure();
  cb.OnFailure();
  ASSERT_EQ(cb.state(), dws::CircuitBreaker::State::kOpen);
  EXPECT_FALSE(cb.Allow());
  SleepMs(120);                              // 冷却结束
  EXPECT_TRUE(cb.Allow());                   // 进入半开，放一个探针
  EXPECT_FALSE(cb.Allow());                  // 半开期间只放一个
  cb.OnSuccess();
  EXPECT_EQ(cb.state(), dws::CircuitBreaker::State::kClosed);
  EXPECT_TRUE(cb.Allow());
}

TEST(CircuitBreaker, HalfOpenFailureReopens) {
  dws::CircuitBreaker cb(2, 80);
  cb.OnFailure();
  cb.OnFailure();
  SleepMs(120);
  ASSERT_TRUE(cb.Allow());   // 半开探针
  cb.OnFailure();            // 探针失败 → 立刻重新打开
  EXPECT_EQ(cb.state(), dws::CircuitBreaker::State::kOpen);
  EXPECT_FALSE(cb.Allow());
}
