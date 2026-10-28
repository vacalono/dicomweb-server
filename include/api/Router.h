#pragma once
// URI 路由：把 DICOMweb 路径映射到 STOW/QIDO/WADO 处理器。
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "util/Metrics.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dws {

class ObjectStore;
class MetadataDao;
class IMetadataDao;
class QidoCache;
class TokenBucket;

// 处理器需要的服务上下文（由 main 注入）
struct ApiContext {
  ObjectStore* store = nullptr;
  IMetadataDao* dao = nullptr;
  QidoCache* cache = nullptr;   // 可为空：未配置 Redis 时按"无缓存"运行
};

class Router {
 public:
  using Args = std::vector<std::string>;
  using Handler = std::function<HttpResponse(const HttpRequest&, const Args&)>;

  void set_context(ApiContext ctx) { ctx_ = ctx; }
  const ApiContext& context() const { return ctx_; }

  void Add(const std::string& method, const std::string& pattern, Handler h);
  HttpResponse Dispatch(const HttpRequest& req);

  // 限流：rps <= 0 表示关闭；突发上限取 max(1, rps/10)
  void set_rate_limit(int rps);
  const Metrics& metrics() const { return metrics_; }

  void Get(const std::string& p, Handler h) { Add("GET", p, std::move(h)); }
  void Post(const std::string& p, Handler h) { Add("POST", p, std::move(h)); }
  void Delete(const std::string& p, Handler h) { Add("DELETE", p, std::move(h)); }

  // 注册 DICOMweb 三组接口（STOW-RS / QIDO-RS / WADO-RS）
  void RegisterDicomweb();

 private:
  struct Route {
    std::string method;
    std::vector<std::string> segs;  // ":uid" 表示通配
    Handler handler;
  };

  HttpResponse DispatchInner(const HttpRequest& req);

  HttpResponse HandleStow(const HttpRequest& req, const Args& args) const;
  HttpResponse HandleQido(const HttpRequest& req, const Args& args) const;
  HttpResponse HandleWado(const HttpRequest& req, const Args& args) const;

  ApiContext ctx_;
  std::vector<Route> routes_;
  mutable Metrics metrics_;   // 只读 handler 里也要计数
  std::unique_ptr<TokenBucket> limiter_;
};

}  // namespace dws
