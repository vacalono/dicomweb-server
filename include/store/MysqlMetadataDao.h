#pragma once
// 元数据访问层：MySQL 8 实现（-DDWS_HAVE_MYSQL=ON 时编译）。
// 与 SQLite 实现共用 IMetadataDao 接口，Schema/索引等价；读写连接分离（写单连接加锁，读每线程一条）。
#include "store/IMetadataDao.h"

#include <mutex>
#include <string>

namespace dws {

class MysqlMetadataDao : public IMetadataDao {
 public:
  struct Config {
    std::string host = "127.0.0.1";
    int port = 3306;
    std::string user = "root";
    std::string password;
    std::string db = "dicomweb";
  };

  // dsn 形如 "host=127.0.0.1;port=3306;user=dws;password=xxx;db=dicomweb"
  static Config ParseDsn(const std::string& dsn);

  explicit MysqlMetadataDao(Config cfg);
  ~MysqlMetadataDao() override;

  bool Init() override;

  bool UpsertStudy(const StudyRecord& r) override;
  bool UpsertSeries(const SeriesRecord& r) override;
  bool UpsertInstance(const InstanceRecord& r) override;

  std::vector<StudyRecord> QueryStudies(const QidoFilter& f) const override;
  std::vector<SeriesRecord> QuerySeries(const std::string& study_uid) const override;
  std::vector<InstanceRecord> QueryInstances(const std::string& series_uid) const override;
  std::vector<InstanceRecord> QueryAllInstances() const override;
  std::optional<InstanceRecord> FindInstance(const std::string& sop_uid) const override;

  bool VerifyConsistency() const override;
  const char* backend() const override { return "mysql"; }

 private:
  void* Connect() const;        // 新建一条连接（调用方负责关闭）
  void* WriteConn();            // 写连接：单条 + mutex_
  void* ReadConn() const;       // 读连接：thread_local
  bool Exec(void* conn, const std::string& sql) const;
  std::string Escape(void* conn, const std::string& s) const;

  Config cfg_;
  mutable std::mutex mu_;
  void* conn_ = nullptr;
};

}  // namespace dws
