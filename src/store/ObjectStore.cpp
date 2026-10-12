// 对象存储：POSIX 实现（项目目标平台为 Linux）。
// 落盘策略：先写 .tmp -> fsync -> rename，保证"要么完整可见，要么不存在"。
#include "store/ObjectStore.h"

#include "util/Logger.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>

namespace dws {

namespace {

std::string Sanitize(const std::string& uid) {
  std::string s = uid;
  for (char& c : s) {
    if (c == '/' || c == '\\' || c == ':' || c == '\0') c = '_';
  }
  return s;
}

// 递归创建目录（已存在视为成功）
bool MakeDirs(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur.push_back(path[i]);
    if (path[i] != '/') continue;
    if (cur.size() <= 1) continue;  // 跳过根 "/"
    if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
  }
  if (!cur.empty() && ::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
  return true;
}

}  // namespace

std::string ObjectStore::PathFor(const std::string& sop_uid) const {
  // 两级散列目录，避免单目录海量文件
  const std::string uid = Sanitize(sop_uid);
  std::string a = uid.size() >= 2 ? uid.substr(0, 2) : "00";
  std::string b = uid.size() >= 4 ? uid.substr(2, 2) : "00";
  return root_ + "/" + a + "/" + b + "/" + uid + ".dcm";
}

// 把数据完整写入指定路径并 fsync（供 WriteTemp 复用）
static bool WriteAll(const std::string& path, const std::string& data, std::string* err) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    if (err) *err = std::strerror(errno);
    return false;
  }
  size_t written = 0;
  while (written < data.size()) {
    ssize_t n = ::write(fd, data.data() + written, data.size() - written);
    if (n <= 0) {
      if (errno == EINTR) continue;
      if (err) *err = std::strerror(errno);
      ::close(fd);
      return false;
    }
    written += static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0) LOG_WARN("fsync(%s) failed: %s", path.c_str(), std::strerror(errno));
  ::close(fd);
  return true;
}

bool ObjectStore::WriteAtomic(const std::string& sop_uid, const std::string& data,
                              std::string* err) {
  const std::string path = PathFor(sop_uid);
  const std::string dir = path.substr(0, path.find_last_of('/'));
  if (!MakeDirs(dir)) {
    if (err) *err = std::strerror(errno);
    return false;
  }

  const std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    if (err) *err = std::strerror(errno);
    return false;
  }

  size_t written = 0;
  while (written < data.size()) {
    ssize_t n = ::write(fd, data.data() + written, data.size() - written);
    if (n <= 0) {
      if (errno == EINTR) continue;
      if (err) *err = std::strerror(errno);
      ::close(fd);
      return false;
    }
    written += static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0) LOG_WARN("fsync(%s) failed: %s", tmp.c_str(), std::strerror(errno));
  ::close(fd);

  if (::rename(tmp.c_str(), path.c_str()) != 0) {  // 原子可见
    if (err) *err = std::strerror(errno);
    return false;
  }
  return true;
}

bool ObjectStore::WriteStream(const std::string& sop_uid, const std::string& data,
                              std::string* err) {
  // 小对象走原子写；超大对象可在此改为分块流式落盘（避免整体驻留内存）
  return WriteAtomic(sop_uid, data, err);
}

bool ObjectStore::WriteTemp(const std::string& data, std::string* out_tmp_path,
                            std::string* err) {
  static std::atomic<uint64_t> seq{0};
  const std::string dir = root_ + "/tmp";
  if (!MakeDirs(dir)) {
    if (err) *err = std::strerror(errno);
    return false;
  }
  std::string path =
      dir + "/upload_" + std::to_string(::getpid()) + "_" + std::to_string(++seq) + ".tmp";
  if (!WriteAll(path, data, err)) return false;
  if (out_tmp_path) *out_tmp_path = path;
  return true;
}

bool ObjectStore::Commit(const std::string& tmp_path, const std::string& sop_uid,
                         std::string* err) {
  const std::string dest = PathFor(sop_uid);
  const std::string dir = dest.substr(0, dest.find_last_of('/'));
  if (!MakeDirs(dir)) {
    if (err) *err = std::strerror(errno);
    return false;
  }
  if (::rename(tmp_path.c_str(), dest.c_str()) != 0) {
    if (err) *err = std::strerror(errno);
    return false;
  }
  return true;
}

bool ObjectStore::Exists(const std::string& sop_uid) const {
  struct stat st{};
  return ::stat(PathFor(sop_uid).c_str(), &st) == 0;
}

bool ObjectStore::Remove(const std::string& sop_uid) const {
  return ::unlink(PathFor(sop_uid).c_str()) == 0;
}

}  // namespace dws
