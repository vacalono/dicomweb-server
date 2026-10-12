#include "net/TcpServer.h"

#include "http/HttpParser.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "net/Buffer.h"
#include "net/Channel.h"
#include "net/EventLoopThreadPool.h"
#include "util/Logger.h"
#include "util/MpscThreadPool.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace dws {

namespace {

void SetNonBlocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

const char* ReasonPhrase(int code) {
  switch (code) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default:  return "OK";
  }
}

// 待发送片段：内存块 或 已打开的只读文件（走 sendfile 零拷贝）。
// 统一成队列，发送到 EAGAIN 时保留进度，等 EPOLLOUT 继续 —— 天然支持背压。
struct SendSeg {
  enum class Kind { kMem, kFile };
  Kind kind = Kind::kMem;
  std::string mem;  // kMem
  int fd = -1;      // kFile：已打开的只读 fd（-1 表示打开失败，直接跳过）
  uint64_t offset = 0;
  uint64_t length = 0;  // 剩余待发字节

  static SendSeg Mem(std::string s) {
    SendSeg seg;
    seg.kind = Kind::kMem;
    seg.mem = std::move(s);
    return seg;
  }
  static SendSeg File(int fd, uint64_t off, uint64_t len) {
    SendSeg seg;
    seg.kind = Kind::kFile;
    seg.fd = fd;
    seg.offset = off;
    seg.length = len;
    return seg;
  }
};

// 计算 multipart/related 的正文总长度（不读文件内容）
[[maybe_unused]] uint64_t MultipartLength(const HttpResponse& resp) {
  uint64_t total = 0;
  for (const auto& p : resp.parts) {
    struct stat st{};
    uint64_t len = p.length;
    if (len == 0 && ::stat(p.file_path.c_str(), &st) == 0) {
      len = (static_cast<uint64_t>(st.st_size) > p.offset) ? static_cast<uint64_t>(st.st_size) - p.offset : 0;
    }
    std::string prefix = "--" + resp.boundary + "\r\nContent-Type: " + p.content_type +
                         "\r\nContent-Length: " + std::to_string(len) + "\r\n\r\n";
    total += prefix.size() + len + 2;  // 尾随 CRLF
  }
  total += ("--" + resp.boundary + "--\r\n").size();
  return total;
}

}  // namespace

struct TcpServer::Connection {
  int fd = -1;
  EventLoop* io_loop = nullptr;      // 归属的从 Reactor（单 Reactor 时即主 loop）
  std::unique_ptr<Channel> ch;
  Buffer in;
  HttpParser parser;
  std::deque<SendSeg> sendq;        // 待发送队列
  std::deque<HttpRequest> pending;  // 已解析、等待 worker 处理的请求（保证同连接串行）
  bool busy = false;                // 是否有请求正在 worker 中处理
  bool close_after_flush = false;   // 发完当前响应后关闭
  bool closing = false;             // 已登记关闭，等待延迟清理
  bool writing = false;             // 是否已注册 EPOLLOUT
};

TcpServer::TcpServer(EventLoop* loop, const Options& opt, HttpHandler handler)
    : loop_(loop), opt_(opt), handler_(std::move(handler)) {
  if (opt_.worker_threads > 0) {
    pool_ = std::make_unique<MpscThreadPool>(opt_.worker_threads);
  }
}

TcpServer::~TcpServer() {
  Stop();
  pool_.reset();  // 等待 worker 收尾
}

bool TcpServer::Start() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    LOG_ERROR("socket() failed: %s", std::strerror(errno));
    return false;
  }

  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(opt_.port));

  if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    LOG_ERROR("bind(:%d) failed: %s", opt_.port, std::strerror(errno));
    return false;
  }
  if (::listen(listen_fd_, opt_.backlog) < 0) {
    LOG_ERROR("listen() failed: %s", std::strerror(errno));
    return false;
  }

  // 从 Reactor 池：必须在开始 accept 之前启动
  if (opt_.io_threads > 0) {
    loop_pool_ = std::make_unique<EventLoopThreadPool>(loop_, opt_.io_threads);
    loop_pool_->Start();
  }

  acceptor_ = std::make_unique<Channel>(loop_, listen_fd_);
  acceptor_->set_read_callback([this] { OnAccept(); });
  acceptor_->EnableReading();

  LOG_INFO("listening on 0.0.0.0:%d (sub reactors=%zu)", opt_.port,
           loop_pool_ ? loop_pool_->size() : 0);
  return true;
}

void TcpServer::Stop() {
  // 1) 停止 accept
  if (acceptor_) {
    acceptor_->DisableAll();
    acceptor_.reset();
  }
  // 2) 停止从 Reactor（Quit + join）；未完成回调随 loop 退出而丢弃
  if (loop_pool_) {
    loop_pool_->Stop();
    loop_pool_.reset();
  }
  // 3) 清理残留连接（此时各 IO loop 已停，只做 fd 回收）
  {
    std::lock_guard<std::mutex> lk(conns_mu_);
    for (auto& kv : conns_) {
      auto& conn = kv.second;
      conn->closing = true;
      for (auto& seg : conn->sendq) {
        if (seg.kind == SendSeg::Kind::kFile && seg.fd >= 0) ::close(seg.fd);
      }
      if (conn->fd >= 0) {
        ::close(conn->fd);
        conn->fd = -1;
      }
    }
    conns_.clear();
  }
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

void TcpServer::OnAccept() {
  // ET：必须循环 accept 直到 EAGAIN，否则残留连接不会被再次通知
  for (;;) {
    struct sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    int fd = ::accept4(listen_fd_, reinterpret_cast<struct sockaddr*>(&peer), &len,
                       SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      LOG_WARN("accept4 failed: %s", std::strerror(errno));
      break;
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    SetNonBlocking(fd);

    // 按轮询把新连接分发给某个从 Reactor
    EventLoop* io_loop = loop_pool_ ? loop_pool_->GetNextLoop() : loop_;
    auto conn = std::make_shared<Connection>();
    conn->fd = fd;
    conn->io_loop = io_loop;
    {
      std::lock_guard<std::mutex> lk(conns_mu_);
      conns_[fd] = conn;
    }
    // Channel 的创建与 epoll 注册必须在连接所属 IO 线程内执行
    Connection* raw = conn.get();
    io_loop->RunInLoop([this, raw] { InitConnection(raw); });
  }
}

void TcpServer::InitConnection(Connection* raw) {
  raw->ch = std::make_unique<Channel>(raw->io_loop, raw->fd);
  raw->ch->set_read_callback([this, raw] { OnMessage(raw, &raw->in); });
  raw->ch->set_close_callback([this, raw] { OnClose(raw); });
  raw->ch->set_error_callback([this, raw] { OnClose(raw); });
  raw->ch->set_write_callback([this, raw] { Flush(raw); });
  raw->ch->EnableReading();
}

void TcpServer::OnMessage(Connection* conn, Buffer* buf) {
  if (conn->closing) return;

  // ET：读到 EAGAIN
  char tmp[64 * 1024];
  for (;;) {
    ssize_t n = ::read(conn->fd, tmp, sizeof(tmp));
    if (n > 0) {
      buf->Append(tmp, static_cast<size_t>(n));
      continue;
    }
    if (n == 0) {  // 对端关闭
      OnClose(conn);
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    if (errno == EINTR) continue;
    OnClose(conn);
    return;
  }

  // 循环解析（可能一次收到多个请求 / 半个请求）
  for (;;) {
    HttpRequest req;
    auto r = conn->parser.Parse(buf, &req);
    if (r == HttpParser::Result::kNeedMore) break;
    if (r == HttpParser::Result::kError) {
      ++total_requests_;
      int code = conn->parser.error_code() ? conn->parser.error_code() : 400;
      conn->close_after_flush = true;
      EnqueueResponse(conn, HttpResponse::Error(code, "malformed request"));
      break;
    }
    ++total_requests_;
    bool close = !req.keep_alive;
    SubmitRequest(conn, std::move(req), close);
    if (close) break;
  }

  Flush(conn);
}

// 请求分发：无 worker 时在 IO 线程同步处理；有 worker 时下沉，且保证同一连接串行
void TcpServer::SubmitRequest(Connection* conn, HttpRequest req, bool close_after) {
  if (close_after) conn->close_after_flush = true;

  if (!pool_) {
    HttpResponse resp = handler_ ? handler_(req) : HttpResponse::Error(500, "no handler");
    EnqueueResponse(conn, resp);
    return;
  }

  conn->pending.push_back(std::move(req));
  if (!conn->busy) ProcessNext(conn);
}

// 必须在 IO 线程调用：取队首请求交给 worker，完成后回到 IO 线程发送并继续下一个
void TcpServer::ProcessNext(Connection* conn) {
  if (conn->closing || conn->pending.empty()) {
    conn->busy = false;
    if (conn->close_after_flush) Flush(conn);  // 队列排空后统一做关闭判断
    return;
  }
  std::shared_ptr<Connection> keep;
  {
    std::lock_guard<std::mutex> lk(conns_mu_);
    auto it = conns_.find(conn->fd);
    if (it == conns_.end()) {  // 连接已销毁
      conn->busy = false;
      return;
    }
    keep = it->second;  // 保活：worker 处理期间对象不被回收
  }
  conn->busy = true;

  EventLoop* io_loop = conn->io_loop;
  auto req = std::make_shared<HttpRequest>(std::move(conn->pending.front()));
  conn->pending.pop_front();

  // 用 shared_ptr 捕获请求，使任务可被拷贝提交（Submit 失败时可回退同步执行）
  auto run = [this, keep, io_loop, req]() {
    HttpResponse resp = handler_ ? handler_(*req) : HttpResponse::Error(500, "no handler");
    io_loop->QueueInLoop([this, keep, resp = std::move(resp)]() {
      Connection* c = keep.get();
      if (!c->closing) {  // 处理期间连接可能已被关闭，此时丢弃响应
        EnqueueResponse(c, resp);
        Flush(c);
      }
      ProcessNext(c);  // 继续处理后续排队请求
    });
  };

  if (pool_ && pool_->Submit(run)) return;

  // 无线程池，或（罕见）无锁队列已满：退回 IO 线程同步执行，保证不丢请求
  if (pool_) LOG_WARN("MpscThreadPool queue full, handling request in IO thread");
  run();
}

void TcpServer::EnqueueResponse(Connection* conn, const HttpResponse& resp) {
  // 1) 先准备 body（可能打开文件 / 计算长度），失败时退回 404 并关闭
  std::deque<SendSeg> body_segs;
  uint64_t body_len = 0;
  bool ok = true;

  if (resp.kind == HttpResponse::BodyKind::kString) {
    body_len = resp.body.size();
    body_segs.push_back(SendSeg::Mem(resp.body));
  } else if (resp.is_file()) {
    struct stat st{};
    uint64_t total = (::stat(resp.file_path.c_str(), &st) == 0)
                         ? static_cast<uint64_t>(st.st_size)
                         : 0;
    body_len = resp.file_length ? resp.file_length
                                : (total > resp.file_offset ? total - resp.file_offset : 0);
    int fd = ::open(resp.file_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      ok = false;
    } else {
      body_segs.push_back(SendSeg::File(fd, resp.file_offset, body_len));
    }
  } else if (resp.is_multipart()) {
    for (const auto& p : resp.parts) {
      struct stat st{};
      uint64_t len = p.length;
      if (len == 0 && ::stat(p.file_path.c_str(), &st) == 0) {
        len = (static_cast<uint64_t>(st.st_size) > p.offset)
                  ? static_cast<uint64_t>(st.st_size) - p.offset
                  : 0;
      }
      std::string prefix = "--" + resp.boundary + "\r\nContent-Type: " + p.content_type +
                           "\r\nContent-Length: " + std::to_string(len) + "\r\n\r\n";
      body_len += prefix.size() + len + 2;
      body_segs.push_back(SendSeg::Mem(std::move(prefix)));
      int fd = ::open(p.file_path.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd >= 0) body_segs.push_back(SendSeg::File(fd, p.offset, len));
      body_segs.push_back(SendSeg::Mem("\r\n"));
    }
    std::string tail = "--" + resp.boundary + "--\r\n";
    body_len += tail.size();
    body_segs.push_back(SendSeg::Mem(std::move(tail)));
  }

  if (!ok) {
    conn->close_after_flush = true;
    const std::string payload = "{\"error\":\"file not found\"}";
    std::string head = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\n"
                       "Content-Length: " + std::to_string(payload.size()) +
                       "\r\nConnection: close\r\n\r\n";
    conn->sendq.push_back(SendSeg::Mem(std::move(head)));
    conn->sendq.push_back(SendSeg::Mem(payload));
    return;
  }

  // 2) 响应头（Content-Length 必须与实际发送字节一致）
  bool close = resp.close_connection || conn->close_after_flush;
  std::string head;
  head.reserve(256);
  head += "HTTP/1.1 " + std::to_string(resp.status) + " " + ReasonPhrase(resp.status) + "\r\n";
  head += "Content-Type: " + resp.content_type + "\r\n";
  head += "Content-Length: " + std::to_string(body_len) + "\r\n";
  head += "Connection: " + std::string(close ? "close" : "keep-alive") + "\r\n";
  for (const auto& kv : resp.headers) head += kv.first + ": " + kv.second + "\r\n";
  head += "\r\n";
  conn->sendq.push_back(SendSeg::Mem(std::move(head)));

  // 3) body 片段
  for (auto& s : body_segs) conn->sendq.push_back(std::move(s));
}

void TcpServer::Flush(Connection* conn) {
  if (conn->closing) return;

  while (!conn->sendq.empty()) {
    SendSeg& seg = conn->sendq.front();

    if (seg.kind == SendSeg::Kind::kMem) {
      size_t sent = 0;
      while (sent < seg.mem.size()) {
        ssize_t n = ::write(conn->fd, seg.mem.data() + sent, seg.mem.size() - sent);
        if (n > 0) {
          sent += static_cast<size_t>(n);
          continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          seg.mem.erase(0, sent);  // 保留剩余，等 EPOLLOUT
          if (!conn->writing) {
            conn->writing = true;
            conn->ch->EnableWriting();
          }
          return;
        }
        if (n < 0 && errno == EINTR) continue;
        OnClose(conn);  // 发送出错：关闭
        return;
      }
      conn->sendq.pop_front();
    } else {  // kFile：sendfile 零拷贝
      int in_fd = seg.fd;
      if (in_fd < 0) {
        conn->sendq.pop_front();
        continue;
      }
      off_t off = static_cast<off_t>(seg.offset);
      uint64_t remaining = seg.length;
      bool done = true;
      while (remaining > 0) {
        ssize_t n = ::sendfile(conn->fd, in_fd, &off, static_cast<size_t>(remaining));
        if (n > 0) {
          remaining -= static_cast<uint64_t>(n);
          continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          seg.offset = static_cast<uint64_t>(off);
          seg.length = remaining;
          if (!conn->writing) {
            conn->writing = true;
            conn->ch->EnableWriting();
          }
          done = false;
          break;
        }
        if (n < 0 && errno == EINTR) continue;
        remaining = 0;  // 其它错误：放弃该文件剩余部分
        break;
      }
      if (!done) return;
      ::close(in_fd);
      seg.fd = -1;
      conn->sendq.pop_front();
    }
  }

  // 队列清空
  if (conn->writing) {
    conn->writing = false;
    conn->ch->DisableWriting();
  }
  // 有请求仍在 worker 中处理或排队时不能关闭，等它们发完
  if (conn->close_after_flush && !conn->busy && conn->pending.empty()) ScheduleClose(conn);
}

void TcpServer::OnClose(Connection* conn) {
  if (!conn || conn->closing) return;
  ScheduleClose(conn);
}

void TcpServer::ScheduleClose(Connection* conn) {
  if (conn->closing) return;
  conn->closing = true;
  if (conn->ch) conn->ch->DisableAll();  // 先从 epoll 摘除，后续不再收到事件
  int fd = conn->fd;
  // 在所属 IO 线程、且延迟到本轮事件处理结束后再销毁，避免回调栈上访问已释放对象
  conn->io_loop->QueueInLoop([this, fd] { DestroyConnection(fd); });
}

void TcpServer::DestroyConnection(int fd) {
  std::shared_ptr<Connection> conn;
  {
    std::lock_guard<std::mutex> lk(conns_mu_);
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    conn = it->second;
    conns_.erase(it);
  }
  for (auto& seg : conn->sendq) {
    if (seg.kind == SendSeg::Kind::kFile && seg.fd >= 0) ::close(seg.fd);
  }
  if (conn->fd >= 0) {
    ::close(conn->fd);
    conn->fd = -1;
  }
}

}  // namespace dws
