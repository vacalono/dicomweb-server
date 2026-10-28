// gRPC 压测客户端：N 线程 × M 次 ListStudies，输出 QPS / p50 / p99。
// 与 HTTP 的 wrk 结果同 runner、同数据、相近并发对比。
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "metadata.grpc.pb.h"

int main(int argc, char** argv) {
  std::string target = "127.0.0.1:50051";
  int threads = 32;
  int per_thread = 500;   // 总请求 = threads * per_thread
  int limit = 100;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* d) { return i + 1 < argc ? std::string(argv[++i]) : std::string(d); };
    if (a == "--target") target = next("127.0.0.1:50051");
    else if (a == "--threads") threads = std::atoi(next("32").c_str());
    else if (a == "--per-thread") per_thread = std::atoi(next("500").c_str());
    else if (a == "--limit") limit = std::atoi(next("100").c_str());
  }

  std::atomic<int> ok{0};
  std::atomic<int> bad{0};
  std::vector<std::vector<double>> lat(threads);

  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> pool;
  pool.reserve(threads);
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t]() {
      auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
      auto stub = dws::rpc::Metadata::NewStub(channel);
      lat[t].reserve(per_thread);
      for (int i = 0; i < per_thread; ++i) {
        dws::rpc::StudyFilter req;
        req.set_limit(limit);
        req.set_offset(0);
        dws::rpc::StudyList resp;
        grpc::ClientContext ctx;
        const auto s = std::chrono::steady_clock::now();
        const grpc::Status st = stub->ListStudies(&ctx, req, &resp);
        const auto e = std::chrono::steady_clock::now();
        if (!st.ok()) {
          ++bad;
          continue;
        }
        ++ok;
        lat[t].push_back(std::chrono::duration<double, std::micro>(e - s).count());
      }
    });
  }
  for (auto& th : pool) th.join();
  const auto elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  std::vector<double> all;
  for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
  std::sort(all.begin(), all.end());
  auto pct = [&](double p) { return all.empty() ? 0.0 : all[static_cast<size_t>(p * (all.size() - 1))]; };

  std::printf("grpc_result target=%s threads=%d ok=%d bad=%d elapsed_s=%.3f qps=%.1f "
              "p50_us=%.0f p99_us=%.0f\n",
              target.c_str(), threads, ok.load(), bad.load(), elapsed,
              static_cast<double>(ok.load()) / elapsed, pct(0.50), pct(0.99));
  return bad.load() == 0 ? 0 : 1;
}
