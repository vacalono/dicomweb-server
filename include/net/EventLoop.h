#pragma once
// Reactor 事件循环：epoll + eventfd 唤醒，支持跨线程投递任务（RunInLoop / QueueInLoop）。
#include "net/EpollPoller.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dws {

class Channel;

class EventLoop {
 public:
  EventLoop();
  ~EventLoop();

  void Loop();               // 阻塞运行，直到 Quit()
  void Quit();

  void RunInLoop(std::function<void()> fn);    // 同线程直接执行，否则入队并唤醒
  void QueueInLoop(std::function<void()> fn);  // 总是入队并唤醒

  bool is_in_loop_thread() const;
  void AssertInLoopThread() const;

  bool AddChannelImpl(Channel* ch);  // 返回是否成功加入 epoll
  void ModChannel(Channel* ch);
  void RemoveChannel(Channel* ch);

  void set_tick_interval_ms(int ms) { tick_interval_ms_ = ms; }  // 时间轮/超时清理
  std::function<void()> on_tick;

 private:
  void Wakeup();
  void HandleWakeup();
  void DoPendingFunctors();

  std::unique_ptr<EpollPoller> poller_;
  int wakeup_fd_ = -1;
  std::atomic<bool> quit_{false};
  const std::thread::id thread_id_;
  std::mutex mu_;
  std::vector<std::function<void()>> pending_;
  int tick_interval_ms_ = 1000;
};

}  // namespace dws
