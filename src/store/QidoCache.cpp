#include "store/QidoCache.h"

#include "util/Logger.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(DWS_HAVE_REDIS)
#if defined(__has_include)
#if __has_include(<hiredis/hiredis.h>)
#include <hiredis/hiredis.h>
#elif __has_include(<hiredis.h>)
#include <hiredis.h>
#endif
#endif
#endif

namespace dws {

#if !defined(DWS_HAVE_REDIS)

std::unique_ptr<QidoCache> CreateQidoCache(const std::string& url) {
  if (!url.empty()) {
    LOG_WARN("Redis cache requested (%s) but binary was built without -DDWS_HAVE_REDIS=ON",
             url.c_str());
  }
  return nullptr;
}

#else  // DWS_HAVE_REDIS

namespace {

struct Endpoint {
  std::string host = "127.0.0.1";
  int port = 6379;
};

Endpoint ParseUrl(const std::string& url) {
  Endpoint ep;
  std::string s = url;
  const std::string prefix = "redis://";
  if (s.rfind(prefix, 0) == 0) s = s.substr(prefix.size());
  const auto colon = s.find(':');
  if (colon == std::string::npos) {
    if (!s.empty()) ep.host = s;
  } else {
    ep.host = s.substr(0, colon);
    ep.port = std::atoi(s.substr(colon + 1).c_str());
  }
  if (ep.host.empty()) ep.host = "127.0.0.1";
  if (ep.port <= 0) ep.port = 6379;
  return ep;
}

class RedisQidoCache : public QidoCache {
 public:
  explicit RedisQidoCache(Endpoint ep) : ep_(std::move(ep)) {}
  ~RedisQidoCache() override = default;

  bool Get(const std::string& key, std::string* out) override {
    redisContext* c = Conn();
    if (!c) return false;
    auto* r = static_cast<redisReply*>(redisCommand(c, "GET %s", key.c_str()));
    if (!r) {
      Reset();
      return false;
    }
    bool ok = false;
    if (r->type == REDIS_REPLY_STRING && out) {
      out->assign(r->str, static_cast<size_t>(r->len));
      ok = true;
      ++hits_;
    } else {
      ++misses_;
    }
    freeReplyObject(r);
    return ok;
  }

  void Set(const std::string& key, const std::string& value, int ttl_seconds) override {
    redisContext* c = Conn();
    if (!c) return;
    auto* r = static_cast<redisReply*>(redisCommand(c, "SETEX %s %d %b", key.c_str(), ttl_seconds,
                                                   value.data(),
                                                   static_cast<size_t>(value.size())));
    if (!r) {
      Reset();
      return;
    }
    freeReplyObject(r);
  }

  void InvalidateStudy(const std::string& study_uid) override {
    DeleteByPattern("qido:v1:studies:*");
    DeleteByPattern("qido:v1:series:" + study_uid + ":*");
  }

  void InvalidateSeries(const std::string& series_uid) override {
    DeleteByPattern("qido:v1:inst:" + series_uid + ":*");
  }

  void Stats(long* hits, long* misses) const override {
    if (hits) *hits = hits_.load();
    if (misses) *misses = misses_.load();
  }

  const char* backend() const override { return "redis"; }

 private:
  redisContext* Conn() {
    if (ctx_) return ctx_;
    struct timeval tv {1, 0};
    ctx_ = redisConnectWithTimeout(ep_.host.c_str(), ep_.port, tv);
    if (!ctx_ || ctx_->err) {
      LOG_WARN("redis connect(%s:%d) failed: %s", ep_.host.c_str(), ep_.port,
               ctx_ ? ctx_->errstr : "oom");
      if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
      }
      return nullptr;
    }
    return ctx_;
  }

  void Reset() {
    if (ctx_) {
      redisFree(ctx_);
      ctx_ = nullptr;
    }
  }

  // 用 SCAN 逐步删除（生产不用 KEYS，会阻塞 Redis 主线程）
  void DeleteByPattern(const std::string& pattern) {
    redisContext* c = Conn();
    if (!c) return;
    std::string cursor = "0";
    int guard = 0;
    do {
      auto* r = static_cast<redisReply*>(
          redisCommand(c, "SCAN %s MATCH %s COUNT 200", cursor.c_str(), pattern.c_str()));
      if (!r || r->type != REDIS_REPLY_ARRAY || r->elements != 2) {
        if (r) freeReplyObject(r);
        Reset();
        return;
      }
      cursor.assign(r->element[0]->str, static_cast<size_t>(r->element[0]->len));
      const redisReply* keys = r->element[1];
      for (size_t i = 0; i < keys->elements; ++i) {
        if (keys->element[i]->type == REDIS_REPLY_STRING) {
          redisCommand(c, "DEL %s", keys->element[i]->str);
        }
      }
      freeReplyObject(r);
      if (++guard > 10000) break;   // 兜底：别在异常数据下无限扫
    } while (cursor != "0");
  }

  Endpoint ep_;
  redisContext* ctx_ = nullptr;   // 每线程一个（对象本身放在线程里用）
  std::atomic<long> hits_{0};
  std::atomic<long> misses_{0};
};

}  // namespace

std::unique_ptr<QidoCache> CreateQidoCache(const std::string& url) {
  if (url.empty()) return nullptr;
  return std::make_unique<RedisQidoCache>(ParseUrl(url));
}

#endif  // DWS_HAVE_REDIS

}  // namespace dws
