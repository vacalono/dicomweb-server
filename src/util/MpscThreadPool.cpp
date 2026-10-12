#include "util/MpscThreadPool.h"

#include "util/Logger.h"

namespace dws {

MpscThreadPool::MpscThreadPool(size_t threads, size_t per_queue_capacity) {
  if (threads == 0) threads = 1;
  workers_.reserve(threads);
  threads_.reserve(threads);
  for (size_t i = 0; i < threads; ++i) {
    workers_.push_back(std::make_unique<Worker>(per_queue_capacity));
  }
  for (auto& w : workers_) {
    Worker* raw = w.get();
    threads_.emplace_back([this, raw] { Run(raw); });
  }
  LOG_INFO("MpscThreadPool started: %zu worker(s), per-queue cap %zu", threads,
           per_queue_capacity);
}

MpscThreadPool::~MpscThreadPool() {
  stop_.store(true, std::memory_order_release);
  for (auto& w : workers_) {
    {
      std::lock_guard<std::mutex> lk(w->mu);
      w->pending = true;  // 唤醒阻塞中的 worker，让其检查 stop_
    }
    w->cv.notify_all();
  }
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
  // 回收未被消费的任务，避免泄漏
  for (auto& w : workers_) {
    while (auto tp = w->queue.Pop()) {
      delete *tp;
    }
  }
}

bool MpscThreadPool::Submit(const Task& task) {
  if (stop_.load(std::memory_order_acquire)) return false;

  // 轮询选 worker（fetch_add 保证各 IO 线程分散到不同队列）
  Worker& w = *workers_[next_.fetch_add(1, std::memory_order_relaxed) % workers_.size()];

  Task* p = new Task(task);  // 入堆：MpscQueue 元素须可平凡拷贝
  if (!w.queue.Push(p)) {
    delete p;
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return false;  // 队列满
  }
  Wake(w);
  return true;
}

void MpscThreadPool::Wake(Worker& w) {
  {
    std::lock_guard<std::mutex> lk(w.mu);
    w.pending = true;  // 与消费者 wait 的谓词配对，避免丢失唤醒
  }
  w.cv.notify_one();
}

void MpscThreadPool::Run(Worker* w) {
  for (;;) {
    // 先无锁取任务
    if (auto tp = w->queue.Pop()) {
      Task* p = *tp;
      if (p) {
        (*p)();
        delete p;
      }
      continue;
    }
    if (stop_.load(std::memory_order_acquire)) return;

    // 队列空：用 mutex + cv 睡眠，避免忙等
    std::unique_lock<std::mutex> lk(w->mu);
    if (!w->pending) {
      w->cv.wait(lk, [&] { return w->pending || stop_.load(std::memory_order_acquire); });
    }
    w->pending = false;  // 回顶部重新尝试 Pop（可能已有新任务）
  }
}

}  // namespace dws
