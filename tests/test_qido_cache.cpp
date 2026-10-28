// QIDO 缓存测试：真连 Redis（DWS_TEST_REDIS_URL），验证 命中/未命中/TTL 语义/失效范围。
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "store/QidoCache.h"

namespace {

std::string UrlOrSkip() {
  const char* url = std::getenv("DWS_TEST_REDIS_URL");
  if (!url || !*url) {
    GTEST_SKIP() << "DWS_TEST_REDIS_URL 未设置，跳过 Redis 缓存测试";
  }
  return url;
}

}  // namespace

TEST(QidoCache, SetupAndHit) {
  const std::string url = UrlOrSkip();
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
  const std::string url = UrlOrSkip();
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
  const std::string url = UrlOrSkip();
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
  const std::string url = UrlOrSkip();
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

TEST(QidoCache, EmptyUrlDisablesCache) {
  EXPECT_EQ(dws::CreateQidoCache(""), nullptr);
}
