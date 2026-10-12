#include "http/HttpParser.h"

#include "util/Logger.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <string_view>
#include <utility>

namespace dws {

namespace {

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string Trim(std::string_view sv) {
  size_t b = 0, e = sv.size();
  while (b < e && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
  return std::string(sv.substr(b, e - b));
}

// 找 CRLF；返回 npos 表示数据不完整（半包）
size_t FindCRLF(std::string_view sv, size_t from = 0) {
  for (size_t i = from; i + 1 < sv.size(); ++i) {
    if (sv[i] == '\r' && sv[i + 1] == '\n') return i;
  }
  return std::string_view::npos;
}

bool ParseUint(std::string_view sv, uint64_t* out) {
  if (sv.empty()) return false;
  uint64_t v = 0;
  auto res = std::from_chars(sv.data(), sv.data() + sv.size(), v);
  if (res.ec != std::errc()) return false;
  *out = v;
  return true;
}

// Range: bytes=start-end（支持 end 省略）
bool ParseRange(std::string_view value, ByteRange* out) {
  constexpr std::string_view kPrefix = "bytes=";
  if (value.size() <= kPrefix.size()) return false;
  if (value.substr(0, kPrefix.size()) != kPrefix) return false;
  std::string_view spec = value.substr(kPrefix.size());
  size_t dash = spec.find('-');
  if (dash == std::string_view::npos) return false;

  uint64_t start = 0, end = 0;
  std::string_view a = spec.substr(0, dash);
  std::string_view b = spec.substr(dash + 1);
  if (!a.empty()) {
    if (!ParseUint(a, &start)) return false;
  }
  if (!b.empty()) {
    if (!ParseUint(b, &end)) return false;
  } else {
    end = UINT64_MAX;  // 到文件尾，由业务层截断
  }
  out->start = start;
  out->end = end;
  return true;
}

}  // namespace

HttpParser::Result HttpParser::ParseRequestLine(std::string_view line, HttpRequest* out) {
  size_t sp1 = line.find(' ');
  if (sp1 == std::string_view::npos) {
    error_code_ = 400;
    error_msg_ = "bad request line";
    return Result::kError;
  }
  size_t sp2 = line.find(' ', sp1 + 1);
  if (sp2 == std::string_view::npos) {
    error_code_ = 400;
    error_msg_ = "bad request line";
    return Result::kError;
  }

  out->method = std::string(line.substr(0, sp1));
  std::string target(line.substr(sp1 + 1, sp2 - sp1 - 1));
  out->version = std::string(line.substr(sp2 + 1));

  if (out->version != "HTTP/1.1" && out->version != "HTTP/1.0") {
    error_code_ = 505;
    error_msg_ = "unsupported version";
    return Result::kError;
  }

  size_t q = target.find('?');
  if (q == std::string::npos) {
    out->path = target;
    return Result::kOk;
  }
  out->path = target.substr(0, q);
  std::string_view query(target.data() + q + 1, target.size() - q - 1);
  size_t pos = 0;
  while (pos <= query.size()) {
    size_t amp = query.find('&', pos);
    std::string_view pair =
        query.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
    if (!pair.empty()) {
      size_t eq = pair.find('=');
      if (eq == std::string_view::npos) {
        out->query[std::string(pair)] = "";
      } else {
        out->query[std::string(pair.substr(0, eq))] = std::string(pair.substr(eq + 1));
      }
    }
    if (amp == std::string_view::npos) break;
    pos = amp + 1;
  }
  return Result::kOk;
}

HttpParser::Result HttpParser::ParseHeaders(std::string_view text, size_t* consumed,
                                            HttpRequest* out) {
  size_t pos = 0;
  for (;;) {
    size_t eol = FindCRLF(text, pos);
    if (eol == std::string_view::npos) return Result::kNeedMore;
    if (eol == pos) {  // 空行 = 头部结束
      *consumed = eol + 2;
      break;
    }
    std::string line = Trim(text.substr(pos, eol - pos));
    pos = eol + 2;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = ToLower(Trim(std::string_view(line).substr(0, colon)));
    std::string val = Trim(std::string_view(line).substr(colon + 1));
    out->headers[key] = val;
  }

  auto it = out->headers.find("content-length");
  if (it != out->headers.end()) {
    uint64_t v = 0;
    if (ParseUint(it->second, &v)) out->content_length = v;
  }
  auto te = out->headers.find("transfer-encoding");
  if (te != out->headers.end() && ToLower(te->second).find("chunked") != std::string::npos) {
    out->is_chunked = true;
  }
  auto conn = out->headers.find("connection");
  if (conn != out->headers.end()) {
    std::string v = ToLower(conn->second);
    out->keep_alive = (v.find("close") == std::string::npos);
  }
  auto rng = out->headers.find("range");
  if (rng != out->headers.end()) {
    ByteRange r;
    if (ParseRange(rng->second, &r)) out->range = r;
  }
  auto ct = out->headers.find("content-type");
  if (ct != out->headers.end()) {
    out->content_type = ct->second;
    size_t b = ToLower(ct->second).find("boundary=");
    if (b != std::string::npos) {
      expected_boundary_ = ct->second.substr(b + 9);
      if (!expected_boundary_.empty() && expected_boundary_.front() == '"') {
        expected_boundary_.erase(0, 1);
        if (!expected_boundary_.empty() && expected_boundary_.back() == '"')
          expected_boundary_.pop_back();
      }
    }
  }
  return Result::kOk;
}

HttpParser::Result HttpParser::Parse(Buffer* buf, HttpRequest* out) {
  // keep-alive：上一个请求已解析完成后，为新请求复位状态机（否则第二个请求不会被解析）
  if (state_ == State::kDone) state_ = State::kRequestLine;

  for (;;) {
    std::string_view sv = buf->Peek();
    if (sv.empty()) return Result::kNeedMore;

    if (state_ == State::kRequestLine) {
      size_t eol = FindCRLF(sv);
      if (eol == std::string_view::npos) {
        // 防御：请求行过长
        if (sv.size() > 8 * 1024) {
          error_code_ = 414;
          error_msg_ = "request line too long";
          return Result::kError;
        }
        return Result::kNeedMore;
      }
      if (ParseRequestLine(sv.substr(0, eol), &partial_) != Result::kOk) return Result::kError;
      buf->Retrieve(eol + 2);
      state_ = State::kHeaders;
      continue;
    }

    if (state_ == State::kHeaders) {
      size_t consumed = 0;
      Result r = ParseHeaders(buf->Peek(), &consumed, &partial_);
      if (r != Result::kOk) return r;
      buf->Retrieve(consumed);
      if (partial_.is_chunked) {
        state_ = State::kChunkSize;
      } else if (partial_.content_length && *partial_.content_length > 0) {
        body_remaining_ = *partial_.content_length;
        state_ = State::kBody;
      } else {
        return FinishRequest(out);
      }
      continue;
    }

    if (state_ == State::kBody) {
      if (sv.size() < body_remaining_) return Result::kNeedMore;  // 半包：等下次 recv
      partial_.body.assign(sv.data(), static_cast<size_t>(body_remaining_));
      buf->Retrieve(static_cast<size_t>(body_remaining_));
      return FinishRequest(out);
    }

    if (state_ == State::kChunkSize) {
      size_t eol = FindCRLF(sv);
      if (eol == std::string_view::npos) return Result::kNeedMore;
      std::string_view line = sv.substr(0, eol);
      size_t semi = line.find(';');
      if (semi != std::string_view::npos) line = line.substr(0, semi);
      uint64_t sz = 0;
      if (!ParseUint(Trim(line), &sz)) {
        error_code_ = 400;
        error_msg_ = "bad chunk size";
        return Result::kError;
      }
      buf->Retrieve(eol + 2);
      if (sz == 0) {
        return FinishRequest(out);
      }
      body_remaining_ = sz;
      state_ = State::kChunkData;
      continue;
    }

    if (state_ == State::kChunkData) {
      if (sv.size() < body_remaining_ + 2) return Result::kNeedMore;
      partial_.body.append(sv.data(), static_cast<size_t>(body_remaining_));
      buf->Retrieve(static_cast<size_t>(body_remaining_) + 2);  // 尾部 CRLF
      state_ = State::kChunkSize;
      continue;
    }

    if (state_ == State::kDone) return FinishRequest(out);
  }
}

HttpParser::Result HttpParser::FinishRequest(HttpRequest* out) {
  ParseMultipartBoundary(&partial_);
  *out = std::move(partial_);  // 完整请求整体交给调用者
  partial_ = HttpRequest{};    // 为新请求（keep-alive 下一个）复位
  state_ = State::kRequestLine;
  error_code_ = 0;
  error_msg_.clear();
  return Result::kOk;
}

// 把完整 body 按 boundary 切分成多个 part（每个 part 含头部 + 内容）。
// 由于 body 已完整收齐，此处无需处理跨包，只需正确跳过边界与尾随 CRLF。
HttpParser::Result HttpParser::ParseMultipartBoundary(HttpRequest* out) {
  out->boundary.clear();
  out->multipart_parts.clear();
  if (expected_boundary_.empty()) return Result::kOk;
  if (out->content_type.find("multipart/") == std::string::npos) return Result::kOk;

  out->boundary = expected_boundary_;
  const std::string delim = "--" + expected_boundary_;
  const std::string& body = out->body;

  size_t pos = body.find(delim);
  if (pos == std::string::npos) return Result::kOk;  // 不是规范 multipart
  pos += delim.size();

  while (pos < body.size()) {
    if (body.compare(pos, 2, "--") == 0) break;  // 结束边界 --boundary--
    size_t eol = body.find("\r\n", pos);
    if (eol == std::string::npos) break;
    pos = eol + 2;

    size_t hdr_end = body.find("\r\n\r\n", pos);
    if (hdr_end == std::string::npos) break;

    MultipartPart part;
    std::string raw_headers = body.substr(pos, hdr_end - pos);
    size_t h = 0;
    while (h < raw_headers.size()) {
      size_t le = raw_headers.find("\r\n", h);
      if (le == std::string::npos) le = raw_headers.size();
      std::string line = raw_headers.substr(h, le - h);
      size_t colon = line.find(':');
      if (colon != std::string::npos) {
        part.headers[ToLower(Trim(std::string_view(line).substr(0, colon)))] =
            Trim(std::string_view(line).substr(colon + 1));
      }
      h = le + 2;
    }

    size_t content_start = hdr_end + 4;
    size_t next = body.find(delim, content_start);
    if (next == std::string::npos) break;
    size_t content_end = next;
    if (content_end >= 2 && body[content_end - 2] == '\r' && body[content_end - 1] == '\n')
      content_end -= 2;
    part.body.assign(body, content_start, content_end - content_start);
    out->multipart_parts.push_back(std::move(part));

    pos = next + delim.size();
  }
  return Result::kOk;
}

}  // namespace dws
