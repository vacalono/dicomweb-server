# 实验记录（P0 基线 / P1 存储层 + 缓存）

> 所有数字都附**机器与参数**；同一 runner 内前后对比才有意义。
> 本机（Windows）跑不了这个服务（epoll 仅 Linux），因此数据全部来自 GitHub Actions `ubuntu-24.04` runner。
> 复现命令见每节末尾；artifact 保留在对应 run 的 `bench-baseline` 里。

## 表 1 · QIDO 基线（sqlite，无缓存）

种子 **1500** 条 study｜wrk 4 线程｜2026-10-28

| 场景 | 连接 | 时长 | QPS | p50 | p99 | 吞吐 | RSS |
|---|---|---|---|---|---|---|---|
| limit=100（小响应） | 200 | 10 s | **12,922** | 14.39 ms | 45.93 ms | 201 MB/s | 11.3 MB |
| limit=1000（大响应） | 100 | 10 s | **2,566** | 33.81 ms | 113.30 ms | 398 MB/s | — |

> 早先 300 条种子时同参数跑出 16.9k QPS（数据量小、SQLite 命中率高），两组不可直接比；**同一 runner 内**才可比。

## 表 2 · 加 Redis 缓存（P1）

同 runner、同一份 1500 条种子；TTL 60 s；命中率用 100 次同查询实测（读 `X-Cache` 头）。

| 场景 | QPS | p50 | p99 | 相对无缓存 |
|---|---|---|---|---|
| limit=100 | **19,744** | 9.40 ms | **28.06 ms** | QPS **+52.8%**｜p50 −34.7%｜p99 **−38.9%** |
| limit=1000 | **8,419** | 12.23 ms | **51.82 ms** | QPS **+228%**｜p50 −63.8%｜p99 **−54.3%** |

**命中率：hit=99 / miss=1 → 99%**

结论：
1. 缓存对**大结果集**收益最大（跳过 JSON 序列化 + 减少读库），p99 砍一半；
2. 小结果集也有收益，但早期 300 条种子时几乎看不出（SQLite 太快、耗时主要在 HTTP 与网络），**收益随响应体增大而放大**；
3. 内存没有明显上涨（RSS 11.3 → 11.0 MB），缓存本体在 Redis 侧。

复现（CI 内同款）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDWS_HAVE_DCMTK=OFF -DDWS_HAVE_REDIS=ON
cmake --build build -j"$(nproc)"
# A 组：无缓存
./build/dicomweb-server --port 8080 --data /tmp/dws-bench &
python3 scripts/bench_prepare.py http://127.0.0.1:8080 1500
bash scripts/bench.sh http://127.0.0.1:8080 10s 200
wrk -t4 -c100 -d10s --latency "http://127.0.0.1:8080/studies?limit=1000"
# B 组：开缓存（Redis 在 6379）
./build/dicomweb-server --port 8080 --data /tmp/dws-bench --redis-url redis://127.0.0.1:6379 &
for i in $(seq 1 100); do curl -s -D - -o /dev/null "http://127.0.0.1:8080/studies?limit=100" | grep -i "^x-cache:"; done | sort | uniq -c
```

## 表 3 · 双后端一致性（P1）

`DaoContract` 同一套断言（三层 Upsert → 过滤查询 → 幂等 → FindInstance → VerifyConsistency）：

| 后端 | 结果 |
|---|---|
| SQLite（默认） | Passed |
| MySQL 8（service container） | **Passed**（真连，不是 skip） |

CI 的 `mysql-redis` job 还做了端到端：STOW → QIDO 首次 `X-Cache: MISS` → 第二次 `HIT` → 再 STOW → 回到 `MISS`。

## 踩坑记录

1. **hiredis 连接不是线程安全的**：第一版把 `redisContext*` 放成缓存对象成员，8 个 worker 共用 → 200 并发压测**段错误**（bench B 直接挂，wrk 只剩 0.2 req/s）。
   修法：连接改 `thread_local`（每线程一条，与 DAO 读连接同一套思路）；加回归用例 `QidoCache.ConcurrentAccessFromManyThreads`（8 线程 × 200 次）。
2. **`GTEST_SKIP()` 不能出现在有返回值的辅助函数里**（GCC 报 void→string 失败），改成 `TryRedisUrl(std::string*)` 返回 bool。
3. **YAML 里 `name: bench (QIDO: sqlite vs ...)` 会被当成嵌套映射**（值是 `QIDO: sqlite`），整串必须加引号。

## 机制说明

- **写连接**：MySQL 单连接 + mutex（写少读多，与 SQLite 一致）。
- **读连接**：`thread_local`，每个 worker 一条，避免连接级锁竞争。
- **索引**：`studies(patient_id, study_date, modality)`、`series(study_uid)`、`instances(series_uid)` —— 与 SQLite 等价。
- **缓存失效**：STOW 成功后才失效（失败不动缓存），按 study 前缀（studies 列表 + 该 study 的 series）与 series 前缀（实例列表）两段 `SCAN` + `DEL`，不用 `KEYS`。
