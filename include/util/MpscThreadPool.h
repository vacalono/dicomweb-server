#pragma once
// 无锁任务池：每个 worker 独占一个 MPSC 队列（多 IO 线程 push，单 worker pop），
// 提交时按轮询选 worker，避免全局锁 + 单一队列的竞争。
//
// 为什么用 MPSC 而非 MPMC：MpscQueue 的语义是"多生产者 / 单消费者"，
// 每个 worker 只从自己的队列取任务，正好满足；多消费者共享一个队列会破坏 its 设计。
//
// 注意：MpscQueue 内部用 std::vector<std::atomic<T>>，要求 T 可平凡拷贝，
// 因此队列元素是 Task*（消费者负责 delete）。
#include "util/MpscQueue.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dws {

class MpscThreadPool {
 public:
  using Task = std::function<void()>;

  explicit MpscThreadPool(size_t threads, size_t per_queue_capacity = 65536);
  ~MpscThreadPool();

  MpscThreadPool(const MpscThreadPool&) = delete;
  MpscThreadPool& operator=(const MpscThreadPool&) = delete;

  // 提交任务；返回 false 表示目标队列已满（调用方可回退到同步处理，不丢请求）
  bool Submit(const Task& task);

  size_t size() const { return workers_.size(); }
  uint64_t rejected() const { return rejected_.load(std::memory_order_relaxed); }

 private:
  struct Worker {
    MpscQueue<Task*> queue;
    std::mutex mu;               // 仅用于"队列空时睡眠 / 有任务时唤醒"
    std::condition_variable cv;
    bool pending = false;        // 在 mu 保护下访问
    explicit Worker(size_t cap) : queue(cap) {}
  };

  void Run(Worker* w);
  void Wake(Worker& w);

  std::vector<std::unique_ptr<Worker>> workers_;
  std::vector<std::thread> threads_;
  std::atomic<size_t> next_{0};      // 轮询游标
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> rejected_{0};
};

}  // namespace dws
