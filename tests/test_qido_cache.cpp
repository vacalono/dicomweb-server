// QIDO 缓存测试：真连 Redis（DWS_TEST_REDIS_URL），验证 命中/未命中/TTL 语义/失效范围。
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "store/QidoCache.h"

namespace {

// 取 Redis 地址；未配置时返回 false，由调用方 GTEST_SKIP()
bool TryRedisUrl(std::string* out) {
  const char* url = std::getenv("DWS_TEST_REDIS_URL");
  if (!url || !*url) return false;
  *out = url;
  return true;
}

}  // namespace

TEST(QidoCache, SetupAndHit) {
  std::string url;
  if (!TryRedisUrl(&url)) GTEST_SKIP() << "DWS_TEST_REDIS_URL 未设置，跳过 Redis 缓存测试";
  auto cache = dws::CreateQidoCache(url);
  ASSERT_NE(cache, nullptr);
  EXPECT_STREQ(cache->backend(), "redis");

  cache->Set("qido:v1:studies:unit-test", "[]", 60);
  std::string out;
  EXPECT_TRUE(cache->Get("qido:v1:studies:unit-test", &out));
  EXPECT_EQ(out, "[]");

  // 未命中
  EXPECT_FALSE(cache->Get("qido:v1:studies:definitely-missing", &out));
}

TEST(QidoCache, InvalidateStudyClearsListAndSeries) {
  std::string url;
  if (!TryRedisUrl(&url)) GTEST_SKIP() << "DWS_TEST_REDIS_URL 未设置，跳过 Redis 缓存测试";
  auto cache = dws::CreateQidoCache(url);

  cache->Set("qido:v1:studies:list", "[1]", 60);
  cache->Set("qido:v1:series:1.2.3:list", "[2]", 60);
  cache->Set("qido:v1:series:9.9.9:list", "[3]", 60);

  cache->InvalidateStudy("1.2.3");

  std::string out;
  EXPECT_FALSE(cache->Get("qido:v1:studies:list", &out)) << "studies 列表应被清掉";
  EXPECT_FALSE(cache->Get("qido:v1:series:1.2.3:list", &out)) << "该 study 的 series 应被清掉";
  EXPECT_TRUE(cache->Get("qido:v1:series:9.9.9:list", &out)) << "别的 study 不该被误删";
  cache->InvalidateStudy("9.9.9");
}

TEST(QidoCache, InvalidateSeriesScopedToThatSeries) {
  std::string url;
  if (!TryRedisUrl(&url)) GTEST_SKIP() << "DWS_TEST_REDIS_URL 未设置，跳过 Redis 缓存测试";
  auto cache = dws::CreateQidoCache(url);

  cache->Set("qido:v1:inst:series-A:list", "[A]", 60);
  cache->Set("qido:v1:inst:series-B:list", "[B]", 60);

  cache->InvalidateSeries("series-A");
  std::string out;
  EXPECT_FALSE(cache->Get("qido:v1:inst:series-A:list", &out));
  EXPECT_TRUE(cache->Get("qido:v1:inst:series-B:list", &out));
  cache->InvalidateSeries("series-B");
}

TEST(QidoCache, StatsCountsHitsAndMisses) {
  std::string url;
  if (!TryRedisUrl(&url)) GTEST_SKIP() << "DWS_TEST_REDIS_URL 未设置，跳过 Redis 缓存测试";
  auto cache = dws::CreateQidoCache(url);
  cache->Set("qido:v1:studies:stats", "x", 60);
  std::string out;
  cache->Get("qido:v1:studies:stats", &out);
  cache->Get("qido:v1:studies:stats-missing", &out);
  long hits = 0, misses = 0;
  cache->Stats(&hits, &misses);
  EXPECT_GE(hits, 1);
  EXPECT_GE(misses, 1);
  cache->InvalidateStudy("x");
}

// 回归：hiredis 的 redisContext 不是线程安全的。
// 曾经多个 worker 共用一个连接，200 并发压测时直接段错误。
TEST(QidoCache, ConcurrentAccessFromManyThreads) {
  std::string url;
  if (!TryRedisUrl(&url)) GTEST_SKIP() << "DWS_TEST_REDIS_URL 未设置";
  auto cache = dws::CreateQidoCache(url);
  const std::string key = "qido:v1:studies:concurrent";
  cache->Set(key, "seed", 60);

  constexpr int kThreads = 8;
  constexpr int kIters = 200;
  std::atomic<int> ok{0}, bad{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&cache, &ok, &bad, &key, t]() {
      for (int i = 0; i < kIters; ++i) {
        std::string out;
        if (cache->Get(key, &out) && out == "seed") {
          ++ok;
        } else {
          ++bad;
        }
        cache->Set("qido:v1:series:t" + std::to_string(t) + ":" + std::to_string(i), "x", 30);
      }
    });
  }
  for (auto& th : threads) th.join();

  EXPECT_EQ(bad.load(), 0) << "并发读出现失败/串数据";
  EXPECT_EQ(ok.load(), kThreads * kIters);
  cache->InvalidateStudy("t0");
}

TEST(QidoCache, EmptyUrlDisablesCache) {
  EXPECT_EQ(dws::CreateQidoCache(""), nullptr);
}
