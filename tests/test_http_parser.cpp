// W1 验收：协议解析单测，覆盖 粘包/半包、keep-alive、chunked、Range、multipart。
#include "http/HttpParser.h"
#include "net/Buffer.h"

#include <gtest/gtest.h>

#include <string>

using namespace dws;

TEST(HttpParser, SimpleGet) {
  Buffer b;
  std::string raw =
      "GET /studies?limit=10 HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: keep-alive\r\n"
      "\r\n";
  b.Append(raw);

  HttpParser p;
  HttpRequest req;
  ASSERT_EQ(p.Parse(&b, &req), HttpParser::Result::kOk);
  EXPECT_EQ(req.method, "GET");
  EXPECT_EQ(req.path, "/studies");
  EXPECT_EQ(req.query["limit"], "10");
  EXPECT_TRUE(req.keep_alive);
  EXPECT_EQ(b.readable(), 0u);
}

TEST(HttpParser, HalfPacketReturnsNeedMore) {
  Buffer b;
  std::string raw =
      "POST /studies HTTP/1.1\r\n"
      "Content-Length: 5\r\n"
      "\r\n"
      "HEL";  // 只到了一半 body
  b.Append(raw);

  HttpParser p;
  HttpRequest req;
  EXPECT_EQ(p.Parse(&b, &req), HttpParser::Result::kNeedMore);
  // 状态机式解析：请求行/头部已消费，缓冲仅保留未收齐的 body 尾巴，等下次 recv 拼接重扫
  EXPECT_EQ(std::string(b.Peek()), "HEL");
}

TEST(HttpParser, StickyPacketTwoRequests) {
  Buffer b;
  std::string r1 = "GET /a HTTP/1.1\r\n\r\n";
  std::string r2 = "GET /b HTTP/1.1\r\n\r\n";
  b.Append(r1 + r2);

  HttpParser p1;
  HttpRequest q1;
  ASSERT_EQ(p1.Parse(&b, &q1), HttpParser::Result::kOk);
  EXPECT_EQ(q1.path, "/a");

  // 同一缓冲中还有第二个请求
  HttpParser p2;
  HttpRequest q2;
  ASSERT_EQ(p2.Parse(&b, &q2), HttpParser::Result::kOk);
  EXPECT_EQ(q2.path, "/b");
}

TEST(HttpParser, RangeHeaderParsed) {
  Buffer b;
  b.Append("GET /studies/s1/series/se1/instances/i1 HTTP/1.1\r\nRange: bytes=100-499\r\n\r\n");
  HttpParser p;
  HttpRequest req;
  ASSERT_EQ(p.Parse(&b, &req), HttpParser::Result::kOk);
  ASSERT_TRUE(req.range.has_value());
  EXPECT_EQ(req.range->start, 100u);
  EXPECT_EQ(req.range->end, 499u);
}

TEST(HttpParser, MultipartBoundaryExtracted) {
  Buffer b;
  b.Append(
      "POST /studies HTTP/1.1\r\n"
      "Content-Type: multipart/related; type=\"application/dicom\"; boundary=MYBOUNDARY\r\n"
      "Content-Length: 0\r\n"
      "\r\n");
  HttpParser p;
  HttpRequest req;
  ASSERT_EQ(p.Parse(&b, &req), HttpParser::Result::kOk);
  EXPECT_NE(req.content_type.find("multipart/related"), std::string::npos);
}

TEST(HttpParser, ChunkedBody) {
  Buffer b;
  b.Append(
      "POST /studies HTTP/1.1\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "5\r\nHELLO\r\n"
      "6\r\n WORLD\r\n"
      "0\r\n\r\n");
  HttpParser p;
  HttpRequest req;
  ASSERT_EQ(p.Parse(&b, &req), HttpParser::Result::kOk);
  EXPECT_EQ(req.body, "HELLO WORLD");
}

// keep-alive：同一个解析器必须能连续解析多个请求（状态机需在完成后复位）
TEST(HttpParser, KeepAliveSecondRequestParsed) {
  Buffer b;
  b.Append("GET /a HTTP/1.1\r\n\r\n");
  HttpParser p;
  HttpRequest q1;
  ASSERT_EQ(p.Parse(&b, &q1), HttpParser::Result::kOk);
  EXPECT_EQ(q1.path, "/a");

  b.Append("GET /b HTTP/1.1\r\n\r\n");
  HttpRequest q2;
  ASSERT_EQ(p.Parse(&b, &q2), HttpParser::Result::kOk);
  EXPECT_EQ(q2.path, "/b");
}

// multipart：STOW-RS 上传多个 DICOM 实例时按 boundary 切分
TEST(HttpParser, MultipartPartsSplit) {
  const std::string boundary = "MYBOUNDARY";
  std::string body;
  body += "--" + boundary + "\r\nContent-Type: application/dicom\r\n\r\nAAAA\r\n";
  body += "--" + boundary + "\r\nContent-Type: application/dicom\r\n\r\nBBBBBB\r\n";
  body += "--" + boundary + "--\r\n";

  Buffer b;
  b.Append("POST /studies HTTP/1.1\r\nContent-Type: multipart/related; boundary=" + boundary +
           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
  HttpParser p;
  HttpRequest req;
  ASSERT_EQ(p.Parse(&b, &req), HttpParser::Result::kOk);
  ASSERT_EQ(req.multipart_parts.size(), 2u);
  EXPECT_EQ(req.multipart_parts[0].body, "AAAA");
  EXPECT_EQ(req.multipart_parts[1].body, "BBBBBB");
  EXPECT_EQ(req.boundary, boundary);
}
