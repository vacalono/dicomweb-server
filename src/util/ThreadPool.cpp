#include "util/ThreadPool.h"

#include "util/Logger.h"

namespace dws {

ThreadPool::ThreadPool(size_t threads) {
  if (threads == 0) threads = 1;
  workers_.reserve(threads);
  for (size_t i = 0; i < threads; ++i) {
    workers_.emplace_back([this] {
      for (;;) {
        std::function<void()> task;
        {
          std::unique_lock<std::mutex> lk(mu_);
          cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
          if (stop_ && tasks_.empty()) return;
          task = std::move(tasks_.front());
          tasks_.pop();
        }
        task();
      }
    });
  }
  LOG_INFO("ThreadPool started with %zu worker(s)", workers_.size());
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  for (auto& t : workers_) {
    if (t.joinable()) t.join();
  }
}

size_t ThreadPool::pending() const {
  std::lock_guard<std::mutex> lk(mu_);
  return tasks_.size();
}

}  // namespace dws
