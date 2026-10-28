# 限流与可观测（P2 第一片）

## 已实现

| 组件 | 位置 | 说明 |
|---|---|---|
| 令牌桶限流 | `include/util/RateLimiter.h` | 全局 QPS 限制，突发上限 = max(1, rps/10)；`--rate-limit <n>` 开启，0 关闭 |
| 限流入口 | `Router::Dispatch` | 超限直接 **429 + `Retry-After: 1`**，不进入业务逻辑（保护下游 DB/缓存）；**`/metrics` 不限流**（过载时监控还要能读） |
| 指标 | `include/util/Metrics.h` | 原子计数 + 延迟直方图，无第三方依赖 |
| 指标端点 | `GET /metrics` | Prometheus 文本格式（`text/plain; version=0.0.4`） |

指标清单：

```
dicomweb_requests_total            请求总数
dicomweb_errors_total              status >= 500 的响应数
dicomweb_rate_limited_total        被限流拒绝的请求数
dicomweb_qido_cache_hits_total     QIDO 缓存命中
dicomweb_qido_cache_misses_total   QIDO 缓存未命中
dicomweb_request_duration_us_*     延迟直方图（1/2/5/10/25/50/100/250ms, +Inf）
```

## CI 实测（`resilience` job）

配置：`--rate-limit 50`，用 `xargs -P20` 并发打 300 个 `GET /studies?limit=10`。

| 指标 | 值 |
|---|---|
| 200 | **33** |
| 429 | **267** |
| `dicomweb_requests_total` | 302（300 业务 + 预热/抓取指标） |
| `dicomweb_rate_limited_total` | 267（与 429 数一致） |
| `dicomweb_errors_total` | 0 |
| 延迟桶 `le="+Inf"` | 301（429 很快返回，绝大多数 < 1 ms） |

结论：限流按预期生效（300 个并发在 50 QPS 下只有约 1/10 通过），指标与状态码一一对应。

## 单测（不依赖网络）

- `TokenBucket.BurstThenRefill`：突发 1 个令牌 → 立刻第二次拒绝 → 150 ms 后恢复
- `TokenBucket.RateIsRespectedOverTime`：50 rps / 200 ms 内放行 6–25 个（含突发）
- `Metrics.CountersAndHistogramRender` / `BucketsAreCumulative`：计数与累计桶正确

## 熔断（P2 第二片）

| 组件 | 位置 | 说明 |
|---|---|---|
| 熔断器 | `include/util/CircuitBreaker.h` | 连续 3 次失败 → 打开；冷却 2 s → 半开放**一个**探针；成功关、失败重开 |
| 健康探测 | `include/util/HealthGuard.h` + `src/util/HealthGuard.cpp` | 后台线程每 500 ms `Ping()` 一次 DAO 与缓存，驱动两个熔断器 |
| 快速失败 | `Router::DispatchInner` | DB 熔断打开 → **503 + Retry-After: 2**，不碰数据库；`/metrics` 不受影响 |
| 缓存降级 | `HandleQido` | 缓存熔断时只跳过缓存（直查库），**不**返回 503 |
| 自愈 | `MysqlMetadataDao::Ping` | `mysql_ping` 失败后**主动重连**，否则 MySQL 恢复后连接一直坏着（这是演练里发现的坑） |

指标新增：

```
dicomweb_db_up                    1/0
dicomweb_cache_up                 1/0
dicomweb_db_breaker{state,failures}
dicomweb_cache_breaker{state,failures}
```

### 演练（CI `drill` job，真 docker MySQL）

| 步骤 | 结果 |
|---|---|
| 正常态 | `GET /studies` → **200**，耗时 1.43 ms；`db_up 1` |
| `docker stop` MySQL 后等 8 s | `GET /studies` → **503**，耗时 **0.19 ms**（快速失败，没等超时）；`db_up 0` |
| `docker start` 并等就绪 | 约 2 s 后 → **200**；`db_up 1`，熔断回到 closed |

结论：断库时**请求不再被拖住**（1.43 ms → 0.19 ms 失败返回），库回来能自动恢复，不需要重启服务。

## 还没做（P2 剩余）

1. **幂等重试上限/退避**：STOW 现在依赖 `ON DUPLICATE KEY` 天然幂等，但没有"失败重试 N 次 + 退避"的显式策略。
2. **Redis 断连演练**：当前只演练了 MySQL；缓存断连的降级路径有实现（跳过缓存），但没有 CI 断言。
3. **注入延迟**（500 ms）场景：熔断目前只看"探测失败"，不感知"变慢"。
4. **`/metrics` 鉴权**：内网裸奔，生产应放内网端口或加 token。
