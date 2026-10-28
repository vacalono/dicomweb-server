# RPC 对照：gRPC/Protobuf vs HTTP/JSON（P5）

## 场景

同一个 runner、同一份 **1500 条 study**、同一个 `limit=100` 查询，读同一张 SQLite 表：

- **HTTP/JSON**：`GET /studies?limit=100`，wrk `-t4 -c32 -d10s`
- **gRPC/Protobuf**：`Metadata.ListStudies`，32 线程同步调用 × 500 次/线程（16,000 次）

## 结果

| 指标 | HTTP/JSON | gRPC/Protobuf |
|---|---|---|
| 吞吐 | **13,386 req/s** | 5,776 req/s |
| p50 | **2.17 ms** | 5.12 ms |
| p99 | **7.41 ms** | 12.07 ms |
| 错误 | 0 | 0（ok=16000） |
| 传输 | 208.73 MB/s | — |

**结论（不美化）：在这个场景里 gRPC 反而慢 2.3×。**

## 为什么（面试要能解释清楚）

1. **这个 HTTP 路径已经被压得很紧了**：自研 HTTP 解析 + 手写 JSON 拼接（无 `ostringstream`、预留 buffer）+ epoll ET + keep-alive + `sendfile`，小结果集上几乎没有多余分配。
2. **gRPC 的固定开销在这个量级压过收益**：HTTP/2 帧、protobuf 编解码、同步 stub 的每次调用开销、服务端线程池调度。响应只有 ~15 KB 时，省下的序列化体积补不上这些固定成本。
3. **两边并发模型不完全对等**：wrk 是 32 条 keep-alive 连接跑在 4 个事件循环线程上；客户端是 32 个线程各自阻塞调用。这会让 gRPC 侧多付线程切换的代价——这点我在结果里如实标注，不假装是"纯协议对比"。

## gRPC 什么时候值得

- 跨语言契约（Protobuf 的 schema 演进比 JSON 约定可靠）；
- 流式（server-streaming 推检测结果）；
- 大 payload + 压缩；
- 服务治理（deadline、重试、拦截器、可观测性共建）；
- 多服务拆分后需要强类型接口。

**不适合**：本项目这条"单机、小响应、已高度优化"的读路径——所以我把它做成**可选 target**（`-DDWS_HAVE_GRPC=ON`），不进主二进制。

## 复现

```bash
cmake -S . -B build -DDWS_BUILD_TESTS=OFF -DDWS_HAVE_DCMTK=OFF -DDWS_HAVE_GRPC=ON
cmake --build build -j"$(nproc)"
./build/dicomweb-server --port 8080 --data /tmp/dws-grpc &      # 先用 bench_prepare 灌 1500 条
wrk -t4 -c32 -d10s --latency "http://127.0.0.1:8080/studies?limit=100"
./build/metadata-grpc-server --addr 127.0.0.1:50051 --db /tmp/dws-grpc/meta.db &
./build/grpc-bench --target 127.0.0.1:50051 --threads 32 --per-thread 500
```
