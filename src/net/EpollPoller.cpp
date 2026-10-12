#include "net/EpollPoller.h"

#include "util/Logger.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace dws {

EpollPoller::EpollPoller(int max_events) : events_(static_cast<size_t>(max_events)) {
  epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epfd_ < 0) LOG_ERROR("epoll_create1 failed: %s", std::strerror(errno));
}

EpollPoller::~EpollPoller() {
  if (epfd_ >= 0) ::close(epfd_);
}

bool EpollPoller::Add(int fd, uint32_t events, void* ptr) {
  struct epoll_event ev{};
  ev.events = events;
  ev.data.ptr = ptr;
  if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
    LOG_ERROR("epoll_ctl ADD fd=%d failed: %s", fd, std::strerror(errno));
    return false;
  }
  return true;
}

bool EpollPoller::Mod(int fd, uint32_t events, void* ptr) {
  struct epoll_event ev{};
  ev.events = events;
  ev.data.ptr = ptr;
  if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
    LOG_ERROR("epoll_ctl MOD fd=%d failed: %s", fd, std::strerror(errno));
    return false;
  }
  return true;
}

bool EpollPoller::Del(int fd) {
  if (::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
    LOG_WARN("epoll_ctl DEL fd=%d failed: %s", fd, std::strerror(errno));
    return false;
  }
  return true;
}

int EpollPoller::Poll(int timeout_ms) {
  int n = ::epoll_wait(epfd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
  if (n < 0) {
    if (errno != EINTR) LOG_ERROR("epoll_wait failed: %s", std::strerror(errno));
    return 0;
  }
  num_events_ = n;
  return n;
}

}  // namespace dws
