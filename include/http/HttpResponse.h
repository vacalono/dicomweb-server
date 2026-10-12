#pragma once
// 响应支持三种 Body 来源：内存字符串、文件（零拷贝 sendfile 路径）、流式回调。
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dws {

class HttpResponse {
 public:
  enum class BodyKind { kNone, kString, kFile, kStream, kMultipart };

  // multipart/related 的一个 part（DICOM 实例文件）
  struct Part {
    std::string file_path;
    uint64_t offset = 0;
    uint64_t length = 0;  // 0 = 读到文件尾
    std::string content_type = "application/dicom";
  };

  int status = 200;
  std::map<std::string, std::string> headers;
  std::string content_type = "application/dicom+json";
  bool close_connection = false;

  BodyKind kind = BodyKind::kNone;
  std::string body;        // kString
  std::string file_path;   // kFile（走 sendfile）
  uint64_t file_offset = 0;
  uint64_t file_length = 0;
  std::function<bool(std::string& chunk)> streamer;  // kStream：返回 false 表示结束

  // kMultipart：series 级 WADO-RS 多实例聚合
  std::string boundary;
  std::vector<Part> parts;

  static HttpResponse Json(const std::string& json, int code = 200) {
    HttpResponse r;
    r.status = code;
    r.content_type = "application/dicom+json";
    r.kind = BodyKind::kString;
    r.body = json;
    return r;
  }

  static HttpResponse Error(int code, const std::string& msg) {
    HttpResponse r;
    r.status = code;
    r.content_type = "application/json";
    r.kind = BodyKind::kString;
    r.body = "{\"error\":\"" + msg + "\"}";
    return r;
  }

  // WADO-RS 下载：整文件零拷贝
  static HttpResponse File(const std::string& path, uint64_t off = 0, uint64_t len = 0) {
    HttpResponse r;
    r.content_type = "application/dicom";
    r.kind = BodyKind::kFile;
    r.file_path = path;
    r.file_offset = off;
    r.file_length = len;  // 0 = 读到文件尾
    return r;
  }

  // WADO-RS series 级：multipart/related 聚合多实例
  static HttpResponse Multipart(const std::string& boundary, std::vector<Part> parts) {
    HttpResponse r;
    r.content_type = "multipart/related; type=\"application/dicom\"; boundary=" + boundary;
    r.kind = BodyKind::kMultipart;
    r.boundary = boundary;
    r.parts = std::move(parts);
    return r;
  }

  bool is_file() const { return kind == BodyKind::kFile; }
  bool is_stream() const { return kind == BodyKind::kStream; }
  bool is_multipart() const { return kind == BodyKind::kMultipart; }
};

}  // namespace dws
