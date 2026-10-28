# 限流与可观测（P2 第一片）

## 已实现

| 组件 | 位置 | 说明 |
|---|---|---|
| 令牌桶限流 | `include/util/RateLimiter.h` | 全局 QPS 限制，突发上限 = max(1, rps/10)；`--rate-limit <n>` 开启，0 关闭 |
| 限流入口 | `Router::Dispatch` | 超限直接 **429 + `Retry-After: 1`**，不进入业务逻辑（保护下游 DB/缓存） |
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

## 还没做（P2 剩余）

1. **熔断**：下游（MySQL/Redis）失败率阈值 + 半开探测；打开时 QIDO/WADO 快速失败（503）而不是等超时。
2. **幂等重试**：STOW 的 instance UID 去重 + 重试上限 + 退避（当前依赖 `ON DUPLICATE KEY` 幂等写）。
3. **故障演练**：kill MySQL / kill Redis / 注入 500 ms 延迟，记录"多久熔断、多久自愈"，写成 runbook。
4. **`/metrics` 的鉴权**：目前内网裸奔，生产应放在内网端口或加 token。
