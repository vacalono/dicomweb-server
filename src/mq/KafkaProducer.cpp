#include "mq/KafkaProducer.h"

#include "util/Logger.h"

#if defined(DWS_HAVE_KAFKA)
#if defined(__has_include)
#if __has_include(<librdkafka/rdkafka.h>)
#include <librdkafka/rdkafka.h>
#else
#include <rdkafka.h>
#endif
#endif
#endif

namespace dws {

#if !defined(DWS_HAVE_KAFKA)

KafkaProducer::~KafkaProducer() = default;

bool KafkaProducer::Init(const std::string&, const std::string&, std::string* err) {
  if (err) *err = "built without -DDWS_HAVE_KAFKA=ON";
  return false;
}
bool KafkaProducer::Publish(const std::string&, const std::string&) { return false; }
void KafkaProducer::Flush(int) {}
void KafkaProducer::OnDelivery(bool ok) {
  if (ok) {
    delivered_.fetch_add(1, std::memory_order_relaxed);
  } else {
    failed_.fetch_add(1, std::memory_order_relaxed);
  }
}

#else  // DWS_HAVE_KAFKA

namespace {
void DeliveryCb(rd_kafka_t*, const rd_kafka_message_t* msg, void* opaque) {
  auto* self = static_cast<KafkaProducer*>(opaque);
  if (!self) return;
  const bool ok = msg->err == 0;
  self->OnDelivery(ok);
  if (!ok) LOG_WARN("kafka delivery failed: %s", rd_kafka_message_errstr(msg));
}
}  // namespace

KafkaProducer::~KafkaProducer() {
  if (rkt_) {
    rd_kafka_topic_destroy(static_cast<rd_kafka_topic_t*>(rkt_));
    rkt_ = nullptr;
  }
  if (rk_) {
    rd_kafka_flush(static_cast<rd_kafka_t*>(rk_), 2000);
    rd_kafka_destroy(static_cast<rd_kafka_t*>(rk_));
    rk_ = nullptr;
  }
}

bool KafkaProducer::Init(const std::string& brokers, const std::string& topic, std::string* err) {
  char errstr[512] = {0};
  rd_kafka_conf_t* conf = rd_kafka_conf_new();
  if (rd_kafka_conf_set(conf, "bootstrap.servers", brokers.c_str(), errstr, sizeof(errstr)) !=
      RD_KAFKA_CONF_OK) {
    if (err) *err = errstr;
    rd_kafka_conf_destroy(conf);
    return false;
  }
  rd_kafka_conf_set(conf, "linger.ms", "5", nullptr, 0);
  rd_kafka_conf_set(conf, "acks", "1", nullptr, 0);
  rd_kafka_conf_set_dr_msg_cb(conf, DeliveryCb);
  rd_kafka_conf_set_opaque(conf, this);

  rd_kafka_t* rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
  if (!rk) {
    if (err) *err = errstr;
    return false;   // 失败时 rdkafka 已释放 conf
  }
  rk_ = rk;
  rkt_ = rd_kafka_topic_new(rk, topic.c_str(), nullptr);
  if (!rkt_) {
    if (err) *err = "rd_kafka_topic_new failed";
    return false;
  }
  LOG_INFO("kafka producer ready: %s (topic=%s)", brokers.c_str(), topic.c_str());
  return true;
}

void KafkaProducer::OnDelivery(bool ok) {
  if (ok) {
    delivered_.fetch_add(1, std::memory_order_relaxed);
  } else {
    failed_.fetch_add(1, std::memory_order_relaxed);
  }
}

bool KafkaProducer::Publish(const std::string& key, const std::string& payload) {
  if (!rk_ || !rkt_) return false;
  std::lock_guard<std::mutex> lock(mu_);
  const int rc = rd_kafka_produce(
      static_cast<rd_kafka_topic_t*>(rkt_), RD_KAFKA_PARTITION_UA, RD_KAFKA_MSG_F_COPY,
      const_cast<char*>(payload.data()), payload.size(), key.data(), key.size(), nullptr);
  if (rc != 0) {
    LOG_WARN("kafka produce failed: %s", rd_kafka_err2str(rd_kafka_last_error()));
    failed_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  rd_kafka_poll(static_cast<rd_kafka_t*>(rk_), 0);   // 驱动投递回调
  return true;
}

void KafkaProducer::Flush(int timeout_ms) {
  if (rk_) rd_kafka_flush(static_cast<rd_kafka_t*>(rk_), timeout_ms);
}

#endif  // DWS_HAVE_KAFKA

}  // namespace dws
