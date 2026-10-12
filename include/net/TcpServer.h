#pragma once
// TCP 服务器：Acceptor(主 loop) + 每连接 Channel + 业务线程池。
// 上传大 body 时写缓冲可能积压，需在 WriteComplete / HighWaterMark 回调中做背压控制。
#include "net/EventLoop.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace dws {

class Buffer;
class Channel;
class EventLoopThreadPool;
class HttpParser;
class HttpResponse;
class MpscThreadPool;

class TcpServer {
 public:
  using HttpHandler = std::function<HttpResponse(const struct HttpRequest&)>;

  struct Options {
    int port = 8080;
    int backlog = 1024;
    size_t io_threads = 0;      // 0 = 单 Reactor；>0 = 主从 Reactor
    size_t worker_threads = 0;  // 0 = hardware_concurrency
    size_t write_high_water = 64 * 1024 * 1024;  // 背压阈值
  };

  TcpServer(EventLoop* loop, const Options& opt, HttpHandler handler);
  ~TcpServer();

  bool Start();
  void Stop();

  uint64_t total_requests() const { return total_requests_.load(std::memory_order_relaxed); }

 private:
  struct Connection;
  void OnAccept();
  void OnMessage(Connection* conn, Buffer* buf);
  void OnClose(Connection* conn);
  void InitConnection(struct Connection* conn);  // 在连接所属 IO 线程内建立 Channel

  // 请求分发：无线程池时在 IO 线程同步处理；有线程池时下沉到 worker，保证同连接串行
  void SubmitRequest(Connection* conn, struct HttpRequest req, bool close_after);
  void ProcessNext(Connection* conn);

  // 发送队列：把 string / file / multipart 统一成可增量发送的片段，天然支持背压
  void EnqueueResponse(Connection* conn, const HttpResponse& resp);
  void Flush(Connection* conn);          // 尽量把 sendq_ 里的数据写出去
  void ScheduleClose(Connection* conn);  // 延迟到本轮事件循环之后真正关闭
  void DestroyConnection(int fd);

  EventLoop* loop_;  // 主 Reactor：只负责 accept
  Options opt_;
  HttpHandler handler_;
  int listen_fd_ = -1;
  std::unique_ptr<Channel> acceptor_;  // 必须与 server 同生命周期，否则 epoll 里悬垂
  std::unique_ptr<EventLoopThreadPool> loop_pool_;  // 从 Reactor 池（opt_.io_threads>0 时创建）
  std::unique_ptr<MpscThreadPool> pool_;            // 无锁业务线程池（opt_.worker_threads>0 时创建）
  std::map<int, std::shared_ptr<Connection>> conns_;  // fd -> 连接（多 IO 线程访问，加锁）
  mutable std::mutex conns_mu_;
  std::atomic<uint64_t> total_requests_{0};
};

}  // namespace dws
