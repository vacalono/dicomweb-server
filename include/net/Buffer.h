#pragma once
// 读写缓冲：处理粘包/半包的基础设施。读侧支持"预留尾巴"以跨包重扫 boundary。
#include <cstring>
#include <string>
#include <vector>

namespace dws {

class Buffer {
 public:
  explicit Buffer(size_t initial = 4096) : data_(initial) {}

  // ---- 写侧（socket -> buffer）----
  void Append(const char* p, size_t n) {
    if (write_ + n > data_.size()) EnsureWritable(n);
    std::memcpy(data_.data() + write_, p, n);
    write_ += n;
  }
  void Append(const std::string& s) { Append(s.data(), s.size()); }
  size_t writable() const { return data_.size() - write_; }

  // ---- 读侧（buffer -> 解析器）----
  const char* read_ptr() const { return data_.data() + read_; }
  size_t readable() const { return write_ - read_; }
  std::string_view Peek() const { return {read_ptr(), readable()}; }

  // 消费 n 字节；n 可小于 readable（保留尾巴给下次 recv 拼接）
  void Retrieve(size_t n) {
    read_ += n;
    if (read_ == write_) read_ = write_ = 0;  // 全部消费后归位
  }
  void RetrieveAll() { read_ = write_ = 0; }

  // 丢弃已消费前缀，把未解析数据挪到头部（跨包重扫前调用）
  void Compact() {
    if (read_ == 0) return;
    std::memmove(data_.data(), data_.data() + read_, readable());
    write_ -= read_;
    read_ = 0;
  }

  std::string RetrieveAllAsString() {
    std::string s(read_ptr(), readable());
    RetrieveAll();
    return s;
  }

 private:
  void EnsureWritable(size_t need) {
    Compact();
    if (write_ + need > data_.size()) data_.resize(write_ + need + data_.size());
  }

  std::vector<char> data_;
  size_t read_ = 0;
  size_t write_ = 0;
};

}  // namespace dws
