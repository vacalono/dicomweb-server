#include "net/EventLoop.h"

#include "net/Channel.h"
#include "util/Logger.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cstring>

namespace dws {

namespace {
constexpr size_t kWakeupTag = 0x1;
}

EventLoop::EventLoop() : poller_(new EpollPoller(1024)), thread_id_(std::this_thread::get_id()) {
  wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  poller_->Add(wakeup_fd_, EPOLLIN, reinterpret_cast<void*>(kWakeupTag));
}

EventLoop::~EventLoop() {
  if (wakeup_fd_ >= 0) ::close(wakeup_fd_);
}

bool EventLoop::is_in_loop_thread() const { return std::this_thread::get_id() == thread_id_; }

void EventLoop::AssertInLoopThread() const {
  if (!is_in_loop_thread()) LOG_ERROR("EventLoop used from wrong thread");
}

void EventLoop::Loop() {
  LOG_INFO("EventLoop start (tid=%zu)", std::hash<std::thread::id>{}(thread_id_));
  while (!quit_.load(std::memory_order_acquire)) {
    int n = poller_->Poll(tick_interval_ms_);
    for (int i = 0; i < n; ++i) {
      struct epoll_event& ev = const_cast<struct epoll_event&>(poller_->events()[i]);
      if (ev.data.ptr == reinterpret_cast<void*>(kWakeupTag)) {
        HandleWakeup();
        continue;
      }
      auto* ch = static_cast<Channel*>(ev.data.ptr);
      if (ch) ch->HandleEvent(ev.events);
    }
    if (n == 0 && on_tick) on_tick();  // 超时清理 / 时间轮
    DoPendingFunctors();
  }
  LOG_INFO("EventLoop stop");
}

void EventLoop::Quit() {
  quit_.store(true, std::memory_order_release);
  Wakeup();
}

void EventLoop::RunInLoop(std::function<void()> fn) {
  if (is_in_loop_thread()) {
    fn();
  } else {
    QueueInLoop(std::move(fn));
  }
}

void EventLoop::QueueInLoop(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    pending_.push_back(std::move(fn));
  }
  Wakeup();
}

void EventLoop::Wakeup() {
  uint64_t one = 1;
  ssize_t n = ::write(wakeup_fd_, &one, sizeof(one));
  (void)n;  // EAGAIN 表示已唤醒，可忽略
}

void EventLoop::HandleWakeup() {
  uint64_t v = 0;
  ssize_t n = ::read(wakeup_fd_, &v, sizeof(v));
  (void)n;
}

void EventLoop::DoPendingFunctors() {
  std::vector<std::function<void()>> fns;
  {
    std::lock_guard<std::mutex> lk(mu_);
    fns.swap(pending_);
  }
  for (auto& f : fns) f();
}

bool EventLoop::AddChannelImpl(Channel* ch) {
  if (!ch) return false;
  // data.ptr 直接存 Channel*，Loop() 分发时据此还原对象
  return poller_->Add(ch->fd(), ch->events(), ch);
}

void EventLoop::ModChannel(Channel* ch) {
  if (ch) poller_->Mod(ch->fd(), ch->events(), ch);
}

void EventLoop::RemoveChannel(Channel* ch) {
  if (ch) poller_->Del(ch->fd());
}

}  // namespace dws
