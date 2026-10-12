// dicomweb-server 入口
#include "api/Router.h"
#include "net/EventLoop.h"
#include "net/TcpServer.h"
#include "store/MetadataDao.h"
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
      "  --db <file>      metadata db (default <data>/meta.db)\n"
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
    else if (a == "--io-threads") io_threads = static_cast<size_t>(std::atoi(next("0").c_str()));
    else if (a == "--workers") workers = static_cast<size_t>(std::atoi(next("0").c_str()));
    else if (a == "--help" || a == "-h") { PrintUsage(argv[0]); return 0; }
    else LOG_WARN("unknown option: %s", a.c_str());
  }
  if (db_path.empty()) db_path = data_dir + "/meta.db";

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
  LOG_INFO("dicomweb-server starting (data=%s, db=%s)", data_dir.c_str(), db_path.c_str());

  // 存储层
  dws::ObjectStore store(data_dir + "/objects");
  dws::MetadataDao dao(db_path);
  if (!dao.Init()) {
    LOG_ERROR("metadata init failed");
    return 1;
  }

  // 路由 + 上下文
  dws::Router router;
  dws::ApiContext ctx;
  ctx.store = &store;
  ctx.dao = &dao;
  router.set_context(ctx);
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
