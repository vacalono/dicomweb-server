# STOW 异步化（Kafka，P3）

## 链路

```
POST /studies ──► 落临时文件 ──► 解析 UID ──► 原子改名（对象落地）
                                      │
                                      ├─ 投递 Kafka（key = sop_uid，保序）
                                      │      └─ 失败 → 回退同步写库（不丢数据）
                                      ▼
                         StowConsumer（group: dws-stow-indexer）
                                      │  幂等 Upsert（ON DUPLICATE KEY）
                                      ├─ 成功 → consumed++
                                      └─ 重试 3 次(100/200/400ms) 仍失败 → DLQ（<topic>.dlq）
```

事件体是 `k=v` 行格式（消费者零 JSON 依赖）：

```
sop_uid=...
series_uid=...
study_uid=...
patient_id=...
patient_name=...
study_date=...
modality=...
file_path=...
size=...
```

## 开关

```bash
./build/dicomweb-server --port 8080 --data ./data \
  --kafka-brokers 127.0.0.1:9092 --kafka-topic dws.stow.events --stow-async
```

不传 `--stow-async` 时行为与之前完全一致（同步写库）。

## CI 实测（`kafka-async` job：docker Kafka 3.8 KRaft 单节点）

| 步骤 | 结果 |
|---|---|
| 启动 | `kafka producer ready` / `kafka consumer ready`（group=dws-stow-indexer）/ `stow async: on` |
| STOW | `{"ok":1,"failed":0}` |
| 最终一致 | **8 s 后 QIDO 能查到**（消费者首次 rebalance 的耗时） |
| 指标 | `stow_async_produced_total 1`、`stow_consumed_total 1`、`fallback 0`、`dead_lettered 0` |

## 取舍与已知问题（面试要能讲）

1. **写后读不立刻可见**：STOW 返回 200 时元数据可能还没入库（当前实测 8 s，主要是消费组首次加入的 rebalance；稳定后通常是毫秒级）。要强一致就关掉 `--stow-async`。
2. **QIDO 缓存可见性**：异步模式下，若消费者入库前 QIDO 已缓存了空结果，最多要等 TTL（60 s）。CI 的异步用例**不开缓存**；生产建议事件驱动失效或对该 key 用更短 TTL。
3. **DLQ 只落盘不告警**：目前只投递到 `<topic>.dlq` 并计数，没有自动重放工具。
4. **Kafka 不可用时**：投递失败会回退同步写库（`fallback` 计数会涨），所以"MQ 挂了"不等于"数据丢了"，但也意味着期间没有削峰能力。
5. **消费组只有一份**：多实例部署会分摊分区（同一实例的事件只被一个消费者处理），这点依赖 `sop_uid` 做 key 保序。

## 未做

- Kafka 断连时的本地队列/重放（现在直接回退同步写）；
- 消费 lag 指标（`rd_kafka_consumer_lag` 需要额外查询，未接 `/metrics`）；
- 事件版本化（现在字段变更只能加不能删）。
