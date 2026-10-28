#include "mq/StowConsumer.h"

#include "mq/KafkaProducer.h"
#include "store/IMetadataDao.h"
#include "util/Logger.h"

#include <chrono>
#include <map>
#include <sstream>
#include <thread>

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

namespace {

// 事件体用最简单的 k=v 行格式：不引入 JSON 依赖，解析成本低、人也能读
std::map<std::string, std::string> ParseKv(const std::string& payload) {
  std::map<std::string, std::string> kv;
  std::istringstream is(payload);
  std::string line;
  while (std::getline(is, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    kv[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return kv;
}

std::string Get(const std::map<std::string, std::string>& kv, const char* k) {
  auto it = kv.find(k);
  return it == kv.end() ? std::string() : it->second;
}

}  // namespace

#if !defined(DWS_HAVE_KAFKA)

StowConsumer::~StowConsumer() = default;
bool StowConsumer::Init(const std::string&, const std::string&, IMetadataDao*, std::string* err) {
  if (err) *err = "built without -DDWS_HAVE_KAFKA=ON";
  return false;
}
void StowConsumer::Start() {}
void StowConsumer::Stop() {}
void StowConsumer::Loop() {}
bool StowConsumer::Handle(const std::string&) { return false; }

#else

StowConsumer::~StowConsumer() { Stop(); }

bool StowConsumer::Init(const std::string& brokers, const std::string& topic, IMetadataDao* dao,
                        std::string* err) {
  if (!dao) {
    if (err) *err = "dao is null";
    return false;
  }
  dao_ = dao;
  topic_ = topic;
  dlq_topic_ = topic + ".dlq";

  char errstr[512] = {0};
  rd_kafka_conf_t* conf = rd_kafka_conf_new();
  auto set = [&](const char* k, const char* v) {
    if (rd_kafka_conf_set(conf, k, v, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
      LOG_WARN("kafka conf %s=%s: %s", k, v, errstr);
    }
  };
  set("bootstrap.servers", brokers.c_str());
  set("group.id", "dws-stow-indexer");
  set("auto.offset.reset", "earliest");
  set("enable.auto.commit", "true");

  rd_kafka_t* rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
  if (!rk) {
    if (err) *err = errstr;
    return false;
  }
  rk_ = rk;
  rd_kafka_poll_set_consumer(rk);

  rd_kafka_topic_partition_list_t* topics = rd_kafka_topic_partition_list_new(1);
  rd_kafka_topic_partition_list_add(topics, topic.c_str(), RD_KAFKA_PARTITION_UA);
  const rd_kafka_resp_err_t sub = rd_kafka_subscribe(rk, topics);
  rd_kafka_topic_partition_list_destroy(topics);
  if (sub != RD_KAFKA_RESP_ERR_NO_ERROR) {
    if (err) *err = rd_kafka_err2str(sub);
    return false;
  }

  auto* dlq = new KafkaProducer();
  std::string derr;
  if (dlq->Init(brokers, dlq_topic_, &derr)) {
    dlq_ = dlq;
  } else {
    LOG_WARN("dlq producer init failed, dead letters will be dropped: %s", derr.c_str());
    delete dlq;
  }
  LOG_INFO("kafka consumer ready: %s (topic=%s, group=dws-stow-indexer)", brokers.c_str(),
           topic.c_str());
  return true;
}

void StowConsumer::Start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this]() { Loop(); });
}

void StowConsumer::Stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  if (rk_) {
    rd_kafka_consumer_close(static_cast<rd_kafka_t*>(rk_));
    rd_kafka_destroy(static_cast<rd_kafka_t*>(rk_));
    rk_ = nullptr;
  }
  if (dlq_) {
    delete static_cast<KafkaProducer*>(dlq_);
    dlq_ = nullptr;
  }
}

void StowConsumer::Loop() {
  while (running_.load()) {
    rd_kafka_message_t* msg = rd_kafka_consumer_poll(static_cast<rd_kafka_t*>(rk_), 100);
    if (!msg) continue;
    if (msg->err) {
      if (msg->err != RD_KAFKA_RESP_ERR__PARTITION_EOF) {
        LOG_WARN("kafka consume error: %s", rd_kafka_message_errstr(msg));
      }
      rd_kafka_message_destroy(msg);
      continue;
    }
    const std::string payload(static_cast<const char*>(msg->payload), msg->len);
    Handle(payload);
    rd_kafka_message_destroy(msg);
  }
}

bool StowConsumer::Handle(const std::string& payload) {
  const auto kv = ParseKv(payload);
  StudyRecord s;
  s.study_uid = Get(kv, "study_uid");
  s.patient_id = Get(kv, "patient_id");
  s.patient_name = Get(kv, "patient_name");
  s.study_date = Get(kv, "study_date");
  s.modality = Get(kv, "modality");

  SeriesRecord se;
  se.series_uid = Get(kv, "series_uid");
  se.study_uid = s.study_uid;
  se.modality = s.modality;

  InstanceRecord in;
  in.sop_uid = Get(kv, "sop_uid");
  in.series_uid = se.series_uid;
  in.file_path = Get(kv, "file_path");
  in.size_bytes = std::atoll(Get(kv, "size").c_str());

  // 重试 3 次（100/200/400ms）：数据库短暂抖动不该直接进 DLQ
  for (int attempt = 0; attempt < 3; ++attempt) {
    const bool ok = dao_->UpsertStudy(s) && dao_->UpsertSeries(se) && dao_->UpsertInstance(in);
    if (ok) {
      consumed_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100 << attempt));
  }

  failed_.fetch_add(1, std::memory_order_relaxed);
  if (dlq_) {
    static_cast<KafkaProducer*>(dlq_)->Publish(in.sop_uid, payload);
    dead_lettered_.fetch_add(1, std::memory_order_relaxed);
    LOG_WARN("stow event dead-lettered: sop=%s", in.sop_uid.c_str());
  }
  return false;
}

#endif  // DWS_HAVE_KAFKA

}  // namespace dws
