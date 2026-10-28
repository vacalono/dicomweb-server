#pragma once
// 元数据访问层：SQLite 实现（默认，零外部依赖）。
// 关键：复合索引 (PatientID, StudyDate, Modality) 提升 QIDO 检索；WAL 保证断电一致性。
// 接口见 IMetadataDao.h；MySQL 8 实现见 MysqlMetadataDao.h。
#include "store/IMetadataDao.h"

#include <string>

struct sqlite3;

namespace dws {

class MetadataDao : public IMetadataDao {
 public:
  explicit MetadataDao(const std::string& db_path);
  ~MetadataDao() override;

  bool Init() override;  // 建表 + 建索引 + PRAGMA journal_mode=WAL

  bool UpsertStudy(const StudyRecord& r) override;
  bool UpsertSeries(const SeriesRecord& r) override;
  bool UpsertInstance(const InstanceRecord& r) override;

  std::vector<StudyRecord> QueryStudies(const QidoFilter& f) const override;
  std::vector<SeriesRecord> QuerySeries(const std::string& study_uid) const override;
  std::vector<InstanceRecord> QueryInstances(const std::string& series_uid) const override;
  std::vector<InstanceRecord> QueryAllInstances() const override;
  std::optional<InstanceRecord> FindInstance(const std::string& sop_uid) const override;

  // 幂等写入：先落对象再写元数据，崩溃后可用此校验修复
  bool VerifyConsistency() const override;
  const char* backend() const override { return "sqlite"; }

 private:
  std::string db_path_;
  sqlite3* db_ = nullptr;

  // 每线程一个独立只读连接：避免多 worker 争抢同一连接（serialized mutex 竞争会拖垮吞吐）。
  // 写操作仍走 db_（单连接 + 上层锁）。
  sqlite3* ReadConn() const;
};

}  // namespace dws
