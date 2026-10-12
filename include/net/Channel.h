#pragma once
// fd 事件封装：把 epoll 事件翻译成 read/write/error/close 回调。
#include "net/EventLoop.h"

#include <cstdint>
#include <functional>

namespace dws {

class Channel {
 public:
  using EventCallback = std::function<void()>;

  Channel(EventLoop* loop, int fd) : loop_(loop), fd_(fd) {}

  int fd() const { return fd_; }
  uint32_t events() const { return events_; }
  bool is_writing() const { return (events_ & EPOLLOUT) != 0; }
  bool is_reading() const { return (events_ & EPOLLIN) != 0; }

  void set_read_callback(EventCallback cb) { read_cb_ = std::move(cb); }
  void set_write_callback(EventCallback cb) { write_cb_ = std::move(cb); }
  void set_error_callback(EventCallback cb) { error_cb_ = std::move(cb); }
  void set_close_callback(EventCallback cb) { close_cb_ = std::move(cb); }

  void EnableReading() {
    events_ |= (EPOLLIN | EPOLLET);
    Update();
  }
  void EnableWriting() {
    events_ |= EPOLLOUT;
    Update();
  }
  void DisableWriting() {
    events_ &= ~static_cast<uint32_t>(EPOLLOUT);
    Update();
  }
  void DisableAll() {
    events_ = 0;
    Update();
  }

  void Remove() { loop_->RemoveChannel(this); }

  void HandleEvent(uint32_t revents) {
    // 错误/挂断优先：登记关闭，交由 close_cb 统一清理，避免重复回调
    if (revents & (EPOLLHUP | EPOLLERR)) {
      if (error_cb_) error_cb_();
      return;
    }
    if (revents & EPOLLIN) {
      if (read_cb_) read_cb_();
    }
    if (revents & EPOLLOUT) {
      if (write_cb_) write_cb_();
    }
    if (revents & EPOLLRDHUP) {
      if (close_cb_) close_cb_();
    }
  }

 private:
  void Update() {
    if (events_ == 0) {
      if (added_) {
        loop_->RemoveChannel(this);
        added_ = false;
      }
    } else if (!added_) {
      added_ = loop_->AddChannelImpl(this);
    } else {
      loop_->ModChannel(this);
    }
  }

  EventLoop* loop_;
  int fd_;
  uint32_t events_ = 0;
  bool added_ = false;
  EventCallback read_cb_, write_cb_, error_cb_, close_cb_;
};

}  // namespace dws
