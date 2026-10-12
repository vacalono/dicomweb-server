#pragma once
// 固定线程数任务池：I/O 线程只做收发，业务下沉到 worker，避免阻塞事件循环。
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace dws {

class ThreadPool {
 public:
  explicit ThreadPool(size_t threads = std::thread::hardware_concurrency());
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // 提交任务，返回 future 以便同步等待（也可 fire-and-forget）
  template <typename F, typename... Args>
  auto Submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
    using Ret = decltype(f(args...));
    auto task = std::make_shared<std::packaged_task<Ret()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    std::future<Ret> fut = task->get_future();
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (stop_) throw std::runtime_error("ThreadPool stopped");
      tasks_.emplace([task] { (*task)(); });
    }
    cv_.notify_one();
    return fut;
  }

  size_t size() const { return workers_.size(); }
  size_t pending() const;

 private:
  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
};

}  // namespace dws
