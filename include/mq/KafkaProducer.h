#pragma once
// Kafka 生产者（librdkafka）：STOW 落盘后投递"实例已落盘"事件，元数据入库交给消费者。
// -DDWS_HAVE_KAFKA=OFF 时 Init() 返回 false，调用方回退同步写库。
#include <atomic>
#include <mutex>
#include <string>

namespace dws {

class KafkaProducer {
 public:
  KafkaProducer() = default;
  ~KafkaProducer();

  bool Init(const std::string& brokers, const std::string& topic, std::string* err);
  bool Ready() const { return rk_ != nullptr; }

  // key 用作分区键：同一实例的消息固定落在同一分区，保持顺序
  bool Publish(const std::string& key, const std::string& payload);
  void Flush(int timeout_ms = 2000);

  void OnDelivery(bool ok);   // 供 librdkafka 投递回调使用

  long delivered() const { return delivered_.load(std::memory_order_relaxed); }
  long failed() const { return failed_.load(std::memory_order_relaxed); }

 private:
  void* rk_ = nullptr;    // rd_kafka_t*
  void* rkt_ = nullptr;   // rd_kafka_topic_t*
  std::mutex mu_;         // 串行化 produce + poll，避免并发 rebalance 抖动
  std::atomic<long> delivered_{0};
  std::atomic<long> failed_{0};
};

}  // namespace dws
