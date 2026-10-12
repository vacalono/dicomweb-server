#pragma once
// 从 Reactor 池：主 Reactor accept 后按轮询把新连接分发给其中一个 EventLoop。
#include <cstddef>
#include <memory>
#include <vector>

namespace dws {

class EventLoop;
class EventLoopThread;

class EventLoopThreadPool {
 public:
  EventLoopThreadPool(EventLoop* base_loop, size_t num_threads);
  ~EventLoopThreadPool();

  EventLoopThreadPool(const EventLoopThreadPool&) = delete;
  EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;

  void Start();
  void Stop();

  // 轮询取下一个从 Reactor（仅在主 Reactor 线程调用）；池为空时回落到主 Reactor
  EventLoop* GetNextLoop();
  size_t size() const { return loops_.size(); }

 private:
  EventLoop* base_loop_;
  size_t num_threads_;
  size_t next_ = 0;
  std::vector<std::unique_ptr<EventLoopThread>> threads_;
  std::vector<EventLoop*> loops_;
};

}  // namespace dws
