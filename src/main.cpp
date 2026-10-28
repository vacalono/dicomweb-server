// dicomweb-server 入口
#include "api/Router.h"
#include "net/EventLoop.h"
#include "net/TcpServer.h"
#include "store/IMetadataDao.h"
#include "store/QidoCache.h"
#include "store/ObjectStore.h"
#include "util/Logger.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <system_error>
#include <string>
#include <thread>

namespace {

dws::EventLoop* g_loop = nullptr;

void OnSignal(int) {
  if (g_loop) g_loop->Quit();
}

void PrintUsage(const char* prog) {
  std::printf(
      "Usage: %s [options]\n"
      "  --port <n>       listen port (default 8080)\n"
      "  --data <dir>     data root (default ./data)\n"
      "  --db <file>      metadata db file, sqlite backend only (default <data>/meta.db)\n"
      "  --db-backend <b> metadata backend: sqlite|mysql (default: env DWS_DB or sqlite)\n"
      "  --mysql-dsn <s>  mysql dsn: host=..;port=..;user=..;password=..;db=..\n"
      "  --redis-url <u>  qido cache: redis://host:port (default: env DWS_REDIS_URL)\n"
      "  --rate-limit <n> global QPS limit for incoming requests (0 = off)\n"
      "  --io-threads <n> sub reactors (default 0 = single reactor)\n"
      "  --workers <n>    business worker threads (default = ncpu; 0 = sync in IO thread)\n"
      "  --help\n",
      prog);
}

}  // namespace

int main(int argc, char** argv) {
  int port = 8080;
  std::string data_dir = "./data";
  std::string db_path;
  std::string db_backend = "sqlite";
  std::string mysql_dsn;
  std::string redis_url;
  int rate_limit = 0;
  size_t io_threads = 0;
  size_t workers = std::thread::hardware_concurrency();
  if (workers == 0) workers = 4;  // 默认按 CPU 核数启用业务线程池；--workers 0 可关为同步模式

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* def) -> std::string {
      return (i + 1 < argc) ? std::string(argv[++i]) : std::string(def);
    };
    if (a == "--port") port = std::atoi(next("8080").c_str());
    else if (a == "--data") data_dir = next("./data");
    else if (a == "--db") db_path = next("");
    else if (a == "--db-backend") db_backend = next("sqlite");
    else if (a == "--mysql-dsn") mysql_dsn = next("");
    else if (a == "--redis-url") redis_url = next("");
    else if (a == "--rate-limit") rate_limit = std::atoi(next("0").c_str());
    else if (a == "--io-threads") io_threads = static_cast<size_t>(std::atoi(next("0").c_str()));
    else if (a == "--workers") workers = static_cast<size_t>(std::atoi(next("0").c_str()));
    else if (a == "--help" || a == "-h") { PrintUsage(argv[0]); return 0; }
    else LOG_WARN("unknown option: %s", a.c_str());
  }
  if (db_path.empty()) db_path = data_dir + "/meta.db";
  if (const char* e = std::getenv("DWS_DB")) db_backend = e;
  if (const char* e = std::getenv("DWS_MYSQL_DSN")) mysql_dsn = e;
  if (const char* e = std::getenv("DWS_REDIS_URL")) redis_url = e;

  dws::Logger::Instance().set_level(dws::LogLevel::kInfo);

  // 确保数据目录存在（否则 SQLite 无法创建 meta.db）
  {
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);
    if (ec) {
      std::fprintf(stderr, "cannot create data dir '%s': %s\n", data_dir.c_str(),
                   ec.message().c_str());
      return 1;
    }
  }
  LOG_INFO("dicomweb-server starting (data=%s, backend=%s, redis=%s)", data_dir.c_str(),
           db_backend.c_str(), redis_url.empty() ? "off" : redis_url.c_str());

  // 存储层
  dws::ObjectStore store(data_dir + "/objects");
  const std::string dsn = (db_backend == "mysql") ? mysql_dsn : db_path;
  auto dao = dws::CreateMetadataDao(db_backend, dsn);
  if (!dao || !dao->Init()) {
    LOG_ERROR("metadata init failed (backend=%s)", db_backend.c_str());
    return 1;
  }
  LOG_INFO("metadata backend: %s", dao->backend());

  // QIDO 结果缓存（可选）
  auto cache = dws::CreateQidoCache(redis_url);
  if (cache) LOG_INFO("qido cache: %s", cache->backend());

  // 路由 + 上下文
  dws::Router router;
  dws::ApiContext ctx;
  ctx.store = &store;
  ctx.dao = dao.get();
  ctx.cache = cache.get();
  router.set_context(ctx);
  router.set_rate_limit(rate_limit);
  router.RegisterDicomweb();

  // 服务器
  dws::EventLoop loop;
  g_loop = &loop;
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  dws::TcpServer::Options opt;
  opt.port = port;
  opt.io_threads = io_threads;
  opt.worker_threads = workers;

  dws::TcpServer server(&loop, opt, [&router](const dws::HttpRequest& req) {
    return router.Dispatch(req);
  });

  if (!server.Start()) {
    LOG_ERROR("server start failed");
    return 1;
  }

  LOG_INFO("ready. try: curl -s http://127.0.0.1:%d/studies", port);
  loop.Loop();

  LOG_INFO("shutting down (served %llu requests)",
           static_cast<unsigned long long>(server.total_requests()));
  server.Stop();
  return 0;
}
