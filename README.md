# 面向 PACS 的医学影像 DICOMweb 服务（C++17）

基于 [DICOMweb (PS3.18)](https://dicom.nema.org/medical/dicom/current/output/html/part18.html) 标准的轻量级医学影像 REST 服务：
影像上传（STOW-RS）、检索（QIDO-RS）、流式下载（WADO-RS）与元数据管理。

> 从零部署步骤见 [`docs/云端运行指南.md`](docs/云端运行指南.md)。

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
├── scripts/                      # 测试/压测/互操作/调试脚本（见「测试 / 压测 / 调优」）
├── docs/                         # 指标记录表.md / 云端运行指南.md / 学习路线.md
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

## 存储与缓存（可选后端）

默认 SQLite（零依赖）；`-DDWS_HAVE_MYSQL=ON` 编译 MySQL 8 后端，`-DDWS_HAVE_REDIS=ON` 编译 QIDO 结果缓存。

```bash
# MySQL + Redis
cmake -S . -B build -DDWS_HAVE_MYSQL=ON -DDWS_HAVE_REDIS=ON -DDWS_HAVE_DCMTK=OFF
cmake --build build -j"$(nproc)"
docker compose up -d          # MySQL 8 + Redis 7
./build/dicomweb-server --port 8080 --data ./data \
  --db-backend mysql --mysql-dsn "host=127.0.0.1;port=3306;user=dws;password=dws;db=dicomweb" \
  --redis-url redis://127.0.0.1:6379
```

- 两个后端跑**同一套契约测试**（`DaoContract`），语义一致；
- 缓存键 `qido:v1:{studies|series|inst}:...`，TTL 60 s，STOW 成功后按 study/series 前缀失效；
- 响应带 `X-Cache: HIT|MISS`，可直接用 curl 验证；
- 实测（同一 CI runner，1500 条种子）：limit=100 时 QPS +53%、p99 −39%；limit=1000 时 QPS +228%、p99 −54%；命中率 99%。详见 `docs/experiments.md`。

## 测试 / 压测 / 调优

```bash
ctest --test-dir build --output-on-failure          # 单测
python3 scripts/e2e_client.py http://127.0.0.1:8080 # 端到端：STOW→QIDO→WADO(含 Range)
python3 scripts/interop_orthanc.py                  # 与 Orthanc 的 DICOMweb 互操作
python3 scripts/verify_concurrency.py 8080 stress   # 并发/pipelining 压测
bash scripts/valgrind_check.sh 8081                 # 内存泄漏检查
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
