#pragma once
// STOW 异步入库消费者：从 Kafka 读"实例已落盘"事件 → 幂等写元数据。
// 重试 3 次（100/200/400ms）仍失败 → 投递到 DLQ；消费失败不阻塞后续消息。
#include <atomic>
#include <string>
#include <thread>

namespace dws {

class IMetadataDao;
class KafkaProducer;

class StowConsumer {
 public:
  StowConsumer() = default;
  ~StowConsumer();

  bool Init(const std::string& brokers, const std::string& topic, IMetadataDao* dao,
            std::string* err);
  void Start();
  void Stop();

  long consumed() const { return consumed_.load(std::memory_order_relaxed); }
  long failed() const { return failed_.load(std::memory_order_relaxed); }
  long dead_lettered() const { return dead_lettered_.load(std::memory_order_relaxed); }

 private:
  void Loop();
  bool Handle(const std::string& payload);   // 解析 + 幂等入库（含重试）

  void* rk_ = nullptr;        // rd_kafka_t*（high-level consumer）
  IMetadataDao* dao_ = nullptr;
  std::string topic_;
  std::string dlq_topic_;
  void* dlq_ = nullptr;       // KafkaProducer*
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::atomic<long> consumed_{0};
  std::atomic<long> failed_{0};
  std::atomic<long> dead_lettered_{0};
};

}  // namespace dws
