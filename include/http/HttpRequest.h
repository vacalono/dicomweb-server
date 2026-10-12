#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dws {

struct ByteRange {
  uint64_t start = 0;
  uint64_t end = 0;  // 闭区间
  bool valid() const { return end >= start; }
};

// 一个 multipart 分段（STOW-RS 上传时每个 part 是一个 DICOM 实例）
struct MultipartPart {
  std::unordered_map<std::string, std::string> headers;  // 小写键
  std::string body;
};

struct HttpRequest {
  std::string method;   // GET / POST / DELETE
  std::string path;     // /studies/{uid}/series/{uid}
  std::string version;  // HTTP/1.1

  std::unordered_map<std::string, std::string> headers;  // 小写化键
  std::unordered_map<std::string, std::string> query;    // limit / offset / includefield ...

  std::string body;  // 小请求；大 body（STOW）建议流式落盘，见 MultipartParser

  bool keep_alive = true;
  bool is_chunked = false;
  std::optional<uint64_t> content_length;
  std::optional<ByteRange> range;

  // multipart/related（DICOM 上传）：boundary + 解析后的各 part
  std::string content_type;
  std::string boundary;
  std::vector<MultipartPart> multipart_parts;

  const std::string* FindHeader(const std::string& key) const {
    auto it = headers.find(key);
    return it == headers.end() ? nullptr : &it->second;
  }
};

}  // namespace dws
