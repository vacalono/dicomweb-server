#pragma once
// 一个 EventLoopThread = 一个线程 + 一个 EventLoop（主从 Reactor 中的"从 Reactor"单元）。
// EventLoop 必须在目标线程内构造，否则 thread_id_ 记录错误会导致 RunInLoop 行为异常。
#include "net/EventLoop.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace dws {

class EventLoopThread {
 public:
  EventLoopThread();
  ~EventLoopThread();

  EventLoopThread(const EventLoopThread&) = delete;
  EventLoopThread& operator=(const EventLoopThread&) = delete;

  // 启动线程并返回其 EventLoop（阻塞至 loop 就绪）
  EventLoop* StartLoop();
  void Stop();

 private:
  void ThreadFunc();

  EventLoop* loop_ = nullptr;                    // 供外部使用（线程内构造）
  std::unique_ptr<EventLoop> loop_holder_;       // 持有权
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
};

}  // namespace dws
