#include "net/EventLoopThreadPool.h"

#include "net/EventLoop.h"
#include "net/EventLoopThread.h"
#include "util/Logger.h"

namespace dws {

EventLoopThreadPool::EventLoopThreadPool(EventLoop* base_loop, size_t num_threads)
    : base_loop_(base_loop), num_threads_(num_threads) {}

EventLoopThreadPool::~EventLoopThreadPool() { Stop(); }

void EventLoopThreadPool::Start() {
  loops_.reserve(num_threads_);
  threads_.reserve(num_threads_);
  for (size_t i = 0; i < num_threads_; ++i) {
    auto t = std::make_unique<EventLoopThread>();
    loops_.push_back(t->StartLoop());
    threads_.push_back(std::move(t));
  }
  LOG_INFO("EventLoopThreadPool: %zu sub reactor(s) started", loops_.size());
}

EventLoop* EventLoopThreadPool::GetNextLoop() {
  if (loops_.empty()) return base_loop_;
  EventLoop* l = loops_[next_];
  next_ = (next_ + 1) % loops_.size();
  return l;
}

void EventLoopThreadPool::Stop() {
  for (auto& t : threads_) t->Stop();
  threads_.clear();
  loops_.clear();
}

}  // namespace dws
