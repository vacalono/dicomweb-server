# 实验记录（P0 基线 / P1 存储层）

> 所有数字都附**机器与参数**，同一行格式固定，便于前后对比。
> 本机（Windows）跑不了这个服务（epoll 仅 Linux），因此基线在 GitHub Actions `ubuntu-24.04` runner 上取；与早期本机数字不可直接比较，只做**同一 runner 内的前后对比**。

## 表 1 · QIDO 基线（sqlite，CI runner）

| 日期 | 提交 | 机器 | 参数 | QPS | p99 | 备注 |
|---|---|---|---|---|---|---|
| 2026-10-28 | （首次 bench job） | ubuntu-24.04 runner | 200 连接 / 10 s / limit=100 | 见 artifact `bench-baseline` | 同左 | 300 条种子数据（pydicom 生成） |

复现（CI 内同款）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDWS_HAVE_DCMTK=OFF
cmake --build build -j"$(nproc)"
./build/dicomweb-server --port 8080 --data /tmp/dws-bench &
python3 scripts/bench_prepare.py http://127.0.0.1:8080 300
bash scripts/bench.sh http://127.0.0.1:8080 10s 200
```

## 表 2 · 存储层（P1，完成后填）

| 配置 | QIDO 命中率 | QIDO p95 | 备注 |
|---|---|---|---|
| sqlite | — | — | 基线 |
| mysql 8 | — | — | 同一批种子数据；EXPLAIN 见 `docs/mysql_explain.md` |
| mysql + redis | — | — | TTL 60 s；STOW 后按 study/series 前缀失效 |

## 机制说明（不是数字，但要写清）

- **写连接**：MySQL 单连接 + mutex（与 SQLite 一致，写少读多）。
- **读连接**：`thread_local`，每个 worker 一条，避免连接级锁竞争。
- **索引**：`studies(patient_id, study_date, modality)` 复合索引；`series(study_uid)`；`instances(series_uid)` —— 与 SQLite 版本等价。
- **缓存失效**：STOW 成功后才失效（失败不失效），按 study 前缀 + series 前缀两段扫（SCAN，不用 KEYS）。
