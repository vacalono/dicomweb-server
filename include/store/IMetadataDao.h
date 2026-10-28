#pragma once
// 元数据访问层接口：上层（Router / main）只依赖这个接口，
// 具体实现有 SQLite（默认，零外部依赖）与 MySQL 8（-DDWS_HAVE_MYSQL=ON）。
#include "store/DaoTypes.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dws {

class IMetadataDao {
 public:
  virtual ~IMetadataDao() = default;

  virtual bool Init() = 0;

  virtual bool UpsertStudy(const StudyRecord& r) = 0;
  virtual bool UpsertSeries(const SeriesRecord& r) = 0;
  virtual bool UpsertInstance(const InstanceRecord& r) = 0;

  virtual std::vector<StudyRecord> QueryStudies(const QidoFilter& f) const = 0;
  virtual std::vector<SeriesRecord> QuerySeries(const std::string& study_uid) const = 0;
  virtual std::vector<InstanceRecord> QueryInstances(const std::string& series_uid) const = 0;
  virtual std::vector<InstanceRecord> QueryAllInstances() const = 0;
  virtual std::optional<InstanceRecord> FindInstance(const std::string& sop_uid) const = 0;

  // 幂等写入：先落对象再写元数据，崩溃后可用此校验修复
  virtual bool VerifyConsistency() const = 0;

  // 诊断用：实现名（"sqlite" / "mysql"）
  virtual const char* backend() const = 0;

  // 健康探测（供熔断器使用）：连不上/查询失败返回 false
  virtual bool Ping() = 0;
};

// 工厂：backend = "sqlite" | "mysql"
// sqlite: dsn 是数据库文件路径
// mysql : dsn 形如 "host=127.0.0.1;port=3306;user=dws;password=...;db=dicomweb"
std::unique_ptr<IMetadataDao> CreateMetadataDao(const std::string& backend, const std::string& dsn);

}  // namespace dws
