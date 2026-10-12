#pragma once
// 元数据访问层：SQLite（起步）/ PostgreSQL（进阶）。
// 关键：复合索引 (PatientID, StudyDate, Modality) 提升 QIDO 检索；WAL 保证断电一致性。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace dws {

struct StudyRecord {
  std::string study_uid;
  std::string patient_id;
  std::string patient_name;
  std::string study_date;
  std::string modality;
  std::string description;
};

struct SeriesRecord {
  std::string series_uid;
  std::string study_uid;
  std::string modality;
  int series_number = 0;
};

struct InstanceRecord {
  std::string sop_uid;
  std::string series_uid;
  std::string file_path;
  int64_t size_bytes = 0;
  int64_t created_at = 0;
};

struct QidoFilter {
  std::optional<std::string> patient_id;
  std::optional<std::string> study_date;
  std::optional<std::string> modality;
  int limit = 100;
  int offset = 0;
};

class MetadataDao {
 public:
  explicit MetadataDao(const std::string& db_path);
  ~MetadataDao();

  bool Init();  // 建表 + 建索引 + PRAGMA journal_mode=WAL

  bool UpsertStudy(const StudyRecord& r);
  bool UpsertSeries(const SeriesRecord& r);
  bool UpsertInstance(const InstanceRecord& r);

  std::vector<StudyRecord> QueryStudies(const QidoFilter& f) const;
  std::vector<SeriesRecord> QuerySeries(const std::string& study_uid) const;
  std::vector<InstanceRecord> QueryInstances(const std::string& series_uid) const;
  std::vector<InstanceRecord> QueryAllInstances() const;
  std::optional<InstanceRecord> FindInstance(const std::string& sop_uid) const;

  // 幂等写入：先落对象再写元数据，崩溃后可用此校验修复
  bool VerifyConsistency() const;

 private:
  std::string db_path_;
  sqlite3* db_ = nullptr;

  // 每线程一个独立只读连接：避免多 worker 争抢同一连接（serialized mutex 竞争会拖垮吞吐）。
  // 写操作仍走 db_（单连接 + 上层锁）。
  sqlite3* ReadConn() const;
};

}  // namespace dws
