#pragma once
// epoll 封装：ET 模式。ET 下必须循环 read/write 到 EAGAIN，否则丢事件。
#include <sys/epoll.h>

#include <cstdint>
#include <vector>

namespace dws {

class EpollPoller {
 public:
  using EventList = std::vector<struct epoll_event>;

  explicit EpollPoller(int max_events = 1024);
  ~EpollPoller();

  EpollPoller(const EpollPoller&) = delete;
  EpollPoller& operator=(const EpollPoller&) = delete;

  bool Add(int fd, uint32_t events, void* ptr);
  bool Mod(int fd, uint32_t events, void* ptr);
  bool Del(int fd);

  // timeout_ms = -1 表示阻塞
  int Poll(int timeout_ms);
  const EventList& events() const { return events_; }
  size_t size() const { return num_events_; }

 private:
  int epfd_ = -1;
  EventList events_;
  int num_events_ = 0;
};

}  // namespace dws
