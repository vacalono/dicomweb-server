# 部署（Docker / compose）

## 镜像

多阶段构建：build 阶段装 `cmake/g++/libsqlite3-dev/libmariadb-dev/libhiredis-dev/librdkafka-dev`，
runtime 用 `debian:bookworm-slim` + 四个运行库（`libsqlite3-0`、`libmariadb3`、`libhiredis0.14`、`librdkafka1`）。

```bash
docker build -t dws:latest .
docker run -d -p 8080:8080 -v dws-data:/data dws:latest
```

**实测镜像体积：90.2 MB**（CI `docker` job，`docker image inspect` 的 `.Size`，含四个运行库）。

> 关注点：`debian:bookworm` 里 hiredis 的运行库叫 `libhiredis0.14`（Ubuntu 24.04 才是 `libhiredis1.1.0`）——第一版镜像就是挂在这个包名上。

## 全栈 compose

`docker-compose.yml` 里四个服务：

| 服务 | 作用 | 健康检查 |
|---|---|---|
| `dws` | 本体（MySQL + Redis + Kafka 异步 STOW） | 由 CI 轮询 `/studies` |
| `mysql` | 元数据（InnoDB，复合索引） | `mysqladmin ping` |
| `redis` | QIDO 结果缓存 | `redis-cli ping` |
| `kafka` | STOW 事件（KRaft 单节点） | `kafka-topics.sh --list` |

```bash
docker compose up -d --build     # 起全栈
curl -s localhost:8080/studies
docker compose down -v
```

`dws` 用 `depends_on: condition: service_healthy` 等三个依赖健康后再启动。

## CI e2e（`docker` job）

| 步骤 | 结果 |
|---|---|
| 构建镜像 | **90.2 MB** |
| `compose up` 后服务可用 | **2 s** |
| STOW 一条实例 | `{"ok":1,"failed":0}` |
| QIDO 最终可见 | **2 s**（含 Redis 缓存路径） |
| 指标 | `stow_async_produced_total 1`、`fallback 0`、`db_up 1`、`cache_up 1` |

## 踩到并修掉的问题

1. **运行阶段包名**：`libhiredis1.1.0` 在 bookworm 不存在 → 改 `libhiredis0.14`。
2. **空结果负缓存**：compose e2e 第一次跑时 QIDO 一直空着——第一次轮询把 `[]` 缓存了 60 s，而消费者还没入库。
   修法：`Router` 里**空结果不写缓存**（`out != "[]"`）。这条对同步模式也无害，对异步模式是必须的。

## 未做

- 没有 K8s manifest / Helm chart（只做单机 compose，不吹集群）；
- 镜像没有做非 root 用户与只读根文件系统加固；
- 没有多架构（arm64）构建。
