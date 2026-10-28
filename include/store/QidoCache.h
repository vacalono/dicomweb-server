#pragma once
// QIDO 查询结果缓存（Redis）。-DDWS_HAVE_REDIS=ON 时启用；未编译或未配置 URL 时返回 nullptr，
// 上层按"无缓存"运行（不改变语义，只是每次都查库）。
#include <memory>
#include <string>

namespace dws {

class QidoCache {
 public:
  virtual ~QidoCache() = default;

  virtual bool Get(const std::string& key, std::string* out) = 0;
  virtual void Set(const std::string& key, const std::string& value, int ttl_seconds) = 0;

  // STOW 写入后失效：studies 列表 + 该 study 下的 series 列表
  virtual void InvalidateStudy(const std::string& study_uid) = 0;
  // 该 series 下的 instances 列表
  virtual void InvalidateSeries(const std::string& series_uid) = 0;

  virtual void Stats(long* hits, long* misses) const = 0;
  virtual bool Ping() = 0;   // 健康探测（熔断器用）
  virtual const char* backend() const = 0;
};

// url 形如 "redis://127.0.0.1:6379" 或 "127.0.0.1:6379"；空串返回 nullptr。
std::unique_ptr<QidoCache> CreateQidoCache(const std::string& url);

}  // namespace dws
