#pragma once
// 自研 HTTP/1.1 解析状态机：处理粘包/半包、keep-alive、chunked、Range、multipart。
// 关键点：解析中途不消费缓冲，靠 Buffer 保留尾巴供下次 recv 拼接重扫。
#include "http/HttpRequest.h"
#include "net/Buffer.h"

#include <string>

namespace dws {

class HttpParser {
 public:
  enum class Result { kOk, kNeedMore, kError };

  HttpParser() = default;

  // 尝试从 buffer 解析出一个完整请求；kNeedMore 时不应 Retrieve
  Result Parse(Buffer* buf, HttpRequest* out);

  int error_code() const { return error_code_; }
  const std::string& error_msg() const { return error_msg_; }

 private:
  enum class State {
    kRequestLine,
    kHeaders,
    kBody,
    kChunkSize,
    kChunkData,
    kTrailer,
    kDone,
  };

  Result ParseRequestLine(std::string_view line, HttpRequest* out);
  Result ParseHeaders(std::string_view text, size_t* consumed, HttpRequest* out);
  Result ParseMultipartBoundary(HttpRequest* out);
  Result FinishRequest(HttpRequest* out);  // 请求收完：切分 multipart + 复位状态机

  State state_ = State::kRequestLine;
  int error_code_ = 0;
  std::string error_msg_;
  uint64_t body_remaining_ = 0;
  std::string expected_boundary_;

  // 跨多次 Parse 调用的中间状态：请求行/头部在第一次调用解析，body 可能在后续调用才收齐。
  // 中间结果必须由 parser 自己保存，否则调用者每次新建的 out 会丢掉已解析的 method/path/headers。
  HttpRequest partial_;
};

}  // namespace dws
