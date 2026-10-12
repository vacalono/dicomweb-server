#include "net/EventLoopThread.h"

#include "util/Logger.h"

namespace dws {

EventLoopThread::EventLoopThread() = default;

EventLoopThread::~EventLoopThread() { Stop(); }

EventLoop* EventLoopThread::StartLoop() {
  thread_ = std::thread([this] { ThreadFunc(); });
  std::unique_lock<std::mutex> lk(mu_);
  cv_.wait(lk, [this] { return loop_ != nullptr; });
  return loop_;
}

void EventLoopThread::ThreadFunc() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    loop_holder_ = std::make_unique<EventLoop>();  // 在目标线程内构造
    loop_ = loop_holder_.get();
  }
  cv_.notify_one();

  loop_->Loop();

  {
    std::lock_guard<std::mutex> lk(mu_);
    loop_holder_.reset();
    loop_ = nullptr;
  }
}

void EventLoopThread::Stop() {
  if (!thread_.joinable()) return;
  EventLoop* lp = nullptr;
  {
    std::lock_guard<std::mutex> lk(mu_);
    lp = loop_;
  }
  if (lp) lp->Quit();
  thread_.join();
}

}  // namespace dws
