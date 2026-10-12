#pragma once
// 对象存储：按 SOPInstanceUID 分层落盘（ab/cd/ef/<uid>.dcm），避免单目录海量文件。
#include <string>

namespace dws {

class ObjectStore {
 public:
  explicit ObjectStore(std::string root) : root_(std::move(root)) {}

  // 生成对象路径；uid 为空则视为临时对象
  std::string PathFor(const std::string& sop_uid) const;

  // 流式写入（STOW 大 body 不整体驻留内存）；返回写入字节数
  bool WriteStream(const std::string& sop_uid, const std::string& data, std::string* err);

  // 原子落盘：先写 .tmp 再 rename，避免半截文件被读到
  bool WriteAtomic(const std::string& sop_uid, const std::string& data, std::string* err);

  // STOW 两步落盘：先写临时文件 -> 解析出 SOPInstanceUID -> Commit 改名为正式对象。
  // 这样即使解析失败也只会留下 tmp 文件，不会污染正式命名空间。
  bool WriteTemp(const std::string& data, std::string* out_tmp_path, std::string* err);
  bool Commit(const std::string& tmp_path, const std::string& sop_uid, std::string* err);

  bool Exists(const std::string& sop_uid) const;
  bool Remove(const std::string& sop_uid) const;

  const std::string& root() const { return root_; }

 private:
  std::string root_;
};

}  // namespace dws
