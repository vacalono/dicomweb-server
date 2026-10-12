# 面向 PACS 的医学影像 DICOMweb 服务（C++17）

基于 [DICOMweb (PS3.18)](https://dicom.nema.org/medical/dicom/current/output/html/part18.html) 标准的轻量级医学影像 REST 服务：
影像上传（STOW-RS）、检索（QIDO-RS）、流式下载（WADO-RS）与元数据管理。

> 指标已实测（2026-10-12），完整数据与测量方法见 [`docs/指标记录表.md`](docs/指标记录表.md)；
> 从零部署步骤见 [`docs/云端运行指南.md`](docs/云端运行指南.md)。

## 实测指标（摘要）

环境：96 核共享云容器（Linux x86_64，内核 5.15）/ loopback / g++ 11.3.0 `-O2 -g` / DCMTK 3.6.6；
数据集：200 study / 200 instance（64×64，约 4 KB）+ 1 个 16 MB 实例。

| 指标 | 实测值 | 说明 |
|---|---|---|
| 并发连接 | **1000** | `wrk -c1000`，无 connect/read/write 错误 |
| QPS（QIDO 小响应） | **1264** | workers=1；workers=8 时降到 342（SQLite 并发读负扩展） |
| QPS（纯网络层，无 DB） | **6339** | 404 路径：主从 Reactor + 无锁任务池的上限 |
| QIDO 延迟 | P50 **0.33 ms** / 单请求 **1.2 ms** | 索引命中；P99 受排队与线程唤醒影响（见指标表） |
| TTFB（WADO 16 MB） | **1.4 ms** | `curl -w %{time_starttransfer}`，sendfile 零拷贝 |
| 下载吞吐（WADO 16 MB） | **约 2.6 GB/s** | loopback 上限远高于此 |
| 内存峰值 | **8.6 MB**（VmHWM） | 不随连接数线性增长 |
| 单测 / 泄漏 | **12/12** / **0** | GoogleTest；Valgrind `--leak-check=full` |

## 技术栈

C++17 / DCMTK / epoll(ET) / 主从 Reactor / 无锁 MPSC 任务池 / SQLite（可选 PostgreSQL）/ CMake / GoogleTest / Valgrind / perf

## 平台要求

- **目标平台：Linux**（epoll / sendfile / eventfd / accept4 均为 Linux 专有）。
- 可跨平台编译的部分：`http/HttpParser`、`util/ThreadPool`、`util/MpscQueue`、`util/Logger`、
  `store/MetadataDao`、`api/Router`（已在 Windows/MinGW + g++ 上通过 `-fsyntax-only` 校验）。
- Windows 上开发建议用 WSL2 或容器；不要在纯 Windows 上构建网络层。

## 目录结构

```
dicomweb-server/
├── CMakeLists.txt
├── cmake/Dependencies.cmake      # 依赖查找与回退
├── include/                      # 头文件
│   ├── net/        网络接入层：EpollPoller / EventLoop / TcpServer / Buffer
│   ├── http/       协议解析：HttpParser / HttpRequest / HttpResponse
│   ├── api/        路由与接口：Router（STOW/QIDO/WADO）
│   ├── dicom/      DICOM 编解码封装（DCMTK）
│   ├── store/      存储层：ObjectStore / MetadataDao
│   └── util/       工具：Logger / MpscQueue / MpscThreadPool / ThreadPool（有锁对照）
├── src/                          # 实现
├── tests/                        # GoogleTest 单测
├── sql/schema.sql                # 元数据库表结构与索引
├── scripts/                      # 测试/压测/互操作/调试脚本（见「测试/压测/调优」）
├── docs/                         # 指标记录表.md（实测数据）/ 云端运行指南.md（从零部署）
└── third_party/                  # 第三方依赖（DCMTK / gtest）
```

## 构建

> 🚀 在全新云端 Linux 上**从零部署**的完整步骤（含实测输出、外部访问、常见问题）见
> [`docs/云端运行指南.md`](docs/云端运行指南.md)。

```bash
# 依赖（Ubuntu）：DCMTK + SQLite3 + gtest
#   sudo apt install libdcmtk-dev libsqlite3-dev libgtest-dev
# 或以 FetchContent 自动拉取 gtest（默认开启）

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 可选开关
#   -DDWS_HAVE_DCMTK=ON/OFF   是否编译 DCMTK 相关代码（未装可关掉先跑通网络层）
#   -DDWS_BUILD_TESTS=ON/OFF
#   -DDWS_USE_POSTGRES=ON/OFF
```

## 运行

```bash
./build/dicomweb-server --port 8080 --data ./data
# 主从 Reactor：主 Reactor accept + N 个从 Reactor 处理已建立连接
./build/dicomweb-server --port 8080 --data ./data --io-threads 4
# 业务线程池默认按 CPU 核数启用（IO 线程只做收发，业务下沉到 worker）
./build/dicomweb-server --port 8080 --data ./data --workers 8
# --workers 0 退化为 IO 线程同步处理（便于对照/定位）
```

## 测试 / 压测 / 调优

```bash
ctest --test-dir build --output-on-failure          # 单测
python3 scripts/e2e_client.py http://127.0.0.1:8080 # 端到端：STOW→QIDO→WADO(含 Range)
python3 scripts/interop_orthanc.py                  # W4：与 Orthanc 的 DICOMweb 互操作
python3 scripts/verify_concurrency.py 8080 stress   # 并发/pipelining 压测
bash scripts/valgrind_check.sh 8081                 # 内存泄漏检查（0 泄漏）
python3 scripts/bench_prepare.py http://127.0.0.1:8080 200  # 准备压测数据
bash scripts/bench_workers.sh 8080                  # worker 数 → QPS 调优曲线
bash scripts/bench_metrics.sh 8080                  # WADO 吞吐/TTFB/内存 实测
perf record -g ./build/dicomweb-server && perf report
python3 scripts/debug_big.py http://127.0.0.1:8080  # 回归：不同 body 大小（1KB~16MB）
python3 scripts/debug_wado.py                       # 调试：Orthanc WADO Accept 要求
```

> CI：`.github/workflows/ci.yml` 含三个 job —— 无 DCMTK 快速构建、带 DCMTK 完整构建、ASan/UBSan 内存检查。

> 互操作前置：`apt install orthanc orthanc-dicomweb` 后启动 Orthanc（HTTP 8042，DICOMweb 挂载于 `/dicom-web`），
> 并用 `pip install pydicom` 生成真实 DICOM。`scripts/interop_orthanc.py` 会用同一套 DICOMweb 流程
> 分别打我们的服务器与 Orthanc，验证 STOW/QIDO/WADO 与幂等重传行为一致。

## 实现路线（6 周）

| 周 | 目标 | 完成标志 |
|---|---|---|
| W1 | HTTP 解析器（keep-alive/Range/multipart）+ CMake + 单测 | 解析器单测全绿 |
| W2 | epoll Reactor + 线程池，跑通 WADO-RS 下载 | curl 能下载真实 DICOM |
| W3 | STOW / QIDO + SQLite + 复合索引 | 上传→检索→下载闭环 |
| W4 | 对接 Orthanc + WAL/幂等重传 | 与 Orthanc 互操作通过 |
| W5 | sendfile 零拷贝 + mmap + 内存池 + 无锁队列 | 指标达标（见指标记录表） |
| W6 | Valgrind/perf 调优 + CI + README 指标 | 0 泄漏、CI 绿 |

> **当前进度**：W1（解析器 keep-alive/Range/multipart）、W2（epoll ET Reactor + WADO 下载）、
> W3（STOW/QIDO + SQLite + 复合索引）、W4（与 Orthanc 的 DICOMweb 互操作）核心已完成；
> W5 的 sendfile 零拷贝 + EAGAIN 背压续传、
> **主从 Reactor**（`--io-threads N`：主 Reactor 只 accept + N 个从 Reactor 轮询分发连接，每从 Reactor 独立线程/epoll）、
> **无锁任务池**（`--workers`：每 worker 一个 MPSC 队列，多 IO 线程轮询提交，避免全局锁；
> handler 下沉 worker，同连接串行保序）已完成。
> 已通过 12 个单测、端到端闭环（STOW→QIDO→WADO→Range，上传/下载内容一致、keep-alive 连接复用、
> 单连接 pipelining、5×200 并发）以及与 Orthanc 的互操作验证。
> W4 过程中发现并修复了一处标准兼容性问题：WADO-RS **实例**端点默认应返回 `multipart/related`（PS3.18），
> 此前返回单体文件，与 Orthanc 不一致。
> **W6 已完成**：GitHub Actions 三档 job（无 DCMTK / 带 DCMTK / ASan-UBSan）、Valgrind 0 泄漏、
> 指标实测见 [`docs/指标记录表.md`](docs/指标记录表.md)。W6 调优中又发现并修复两个真实缺陷：
> ① 大 body（>100KB）请求因 `HttpParser` 跨多次 `Parse` 丢失中间状态而全部失败；
> ② 多 worker 因 SQLite 单连接争抢反而更慢（改为每线程独立只读连接，并得出"查询密集负载用更少 worker 更优"的结论）。

## 简历使用的数值（已回填实测）

| 指标 | 简历写法 | 实测值 |
|---|---|---|
| 并发连接 | 1000 并发连接无错误 | 1000（无 connect/read/write 错误） |
| QPS | 纯网络层约 6.3k QPS | 6339（404 路径）/ QIDO 1264 |
| TTFB | 1.4 ms | 1.4 ms（16 MB 实例，sendfile） |
| 下载吞吐 | 约 2.6 GB/s | 约 2.6 GB/s（loopback） |
| 内存 | 进程内存峰值 8.6 MB | 8.6 MB（VmHWM） |
| 检索延迟 | 单请求 1.2 ms / P50 0.33 ms | 1.2 ms / 0.33 ms |

> 逐字对应关系与「已删除的不实主张」（mmap / 内存池 / 35% 下降）见
> [`docs/指标记录表.md`](docs/指标记录表.md) §简历已用数值。
