#include "store/MysqlMetadataDao.h"

#include "util/Logger.h"

#include <sys/stat.h>

#include <chrono>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#if defined(DWS_HAVE_MYSQL)
#if defined(__has_include)
#if __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#elif __has_include(<mysql.h>)
#include <mysql.h>
#else
#error "mysql.h not found: install libmariadb-dev / libmysqlclient-dev"
#endif
#endif
#endif

namespace dws {

namespace {

int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS studies (
  study_uid    VARCHAR(64)  NOT NULL,
  patient_id   VARCHAR(64)  NOT NULL,
  patient_name VARCHAR(128) NULL,
  study_date   CHAR(8)      NULL,
  modality     VARCHAR(16)  NULL,
  description  VARCHAR(255) NULL,
  PRIMARY KEY (study_uid),
  KEY idx_study_lookup (patient_id, study_date, modality)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS series (
  series_uid    VARCHAR(64) NOT NULL,
  study_uid     VARCHAR(64) NOT NULL,
  modality      VARCHAR(16) NULL,
  series_number INT         NULL,
  PRIMARY KEY (series_uid),
  KEY idx_series_study (study_uid),
  CONSTRAINT fk_series_study FOREIGN KEY (study_uid) REFERENCES studies(study_uid) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS instances (
  sop_uid    VARCHAR(64)  NOT NULL,
  series_uid VARCHAR(64)  NOT NULL,
  file_path  VARCHAR(512) NOT NULL,
  size_bytes BIGINT       NULL,
  created_at BIGINT       NULL,
  PRIMARY KEY (sop_uid),
  KEY idx_inst_series (series_uid),
  CONSTRAINT fk_inst_series FOREIGN KEY (series_uid) REFERENCES series(series_uid) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
)SQL";

}  // namespace

MysqlMetadataDao::Config MysqlMetadataDao::ParseDsn(const std::string& dsn) {
  Config c;
  std::stringstream ss(dsn);
  std::string item;
  while (std::getline(ss, item, ';')) {
    const auto eq = item.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = item.substr(0, eq);
    const std::string v = item.substr(eq + 1);
    if (k == "host") c.host = v;
    else if (k == "port") c.port = std::atoi(v.c_str());
    else if (k == "user") c.user = v;
    else if (k == "password") c.password = v;
    else if (k == "db") c.db = v;
  }
  return c;
}

MysqlMetadataDao::MysqlMetadataDao(Config cfg) : cfg_(std::move(cfg)) {}

MysqlMetadataDao::~MysqlMetadataDao() {
#if defined(DWS_HAVE_MYSQL)
  if (conn_) {
    mysql_close(static_cast<MYSQL*>(conn_));
    conn_ = nullptr;
  }
#endif
}

#if !defined(DWS_HAVE_MYSQL)

// 未编译 MySQL 支持：所有操作安全失败（工厂不会创建这个对象）
bool MysqlMetadataDao::Init() {
  LOG_ERROR("MySQL backend not compiled in (configure with -DDWS_HAVE_MYSQL=ON)");
  return false;
}
bool MysqlMetadataDao::UpsertStudy(const StudyRecord&) { return false; }
bool MysqlMetadataDao::UpsertSeries(const SeriesRecord&) { return false; }
bool MysqlMetadataDao::UpsertInstance(const InstanceRecord&) { return false; }
std::vector<StudyRecord> MysqlMetadataDao::QueryStudies(const QidoFilter&) const { return {}; }
std::vector<SeriesRecord> MysqlMetadataDao::QuerySeries(const std::string&) const { return {}; }
std::vector<InstanceRecord> MysqlMetadataDao::QueryInstances(const std::string&) const { return {}; }
std::vector<InstanceRecord> MysqlMetadataDao::QueryAllInstances() const { return {}; }
std::optional<InstanceRecord> MysqlMetadataDao::FindInstance(const std::string&) const { return std::nullopt; }
bool MysqlMetadataDao::VerifyConsistency() const { return false; }
bool MysqlMetadataDao::Ping() { return false; }
void* MysqlMetadataDao::Connect() const { return nullptr; }
void* MysqlMetadataDao::WriteConn() { return nullptr; }
void* MysqlMetadataDao::ReadConn() const { return nullptr; }
bool MysqlMetadataDao::Exec(void*, const std::string&) const { return false; }
std::string MysqlMetadataDao::Escape(void*, const std::string& s) const { return s; }

#else  // DWS_HAVE_MYSQL

void* MysqlMetadataDao::Connect() const {
  MYSQL* c = mysql_init(nullptr);
  if (!c) return nullptr;
  unsigned int timeout = 2;
  mysql_options(c, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  mysql_options(c, MYSQL_SET_CHARSET_NAME, "utf8mb4");
  if (!mysql_real_connect(c, cfg_.host.c_str(), cfg_.user.c_str(), cfg_.password.c_str(),
                          cfg_.db.c_str(), static_cast<unsigned int>(cfg_.port), nullptr, 0)) {
    LOG_ERROR("mysql connect(%s:%d db=%s) failed: %s", cfg_.host.c_str(), cfg_.port,
              cfg_.db.c_str(), mysql_error(c));
    mysql_close(c);
    return nullptr;
  }
  return c;
}

void* MysqlMetadataDao::WriteConn() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!conn_) conn_ = Connect();
  return conn_;
}

void* MysqlMetadataDao::ReadConn() const {
  struct Tls {
    MYSQL* conn = nullptr;
    const MysqlMetadataDao* owner = nullptr;
  };
  static thread_local Tls tls;
  if (tls.conn == nullptr || tls.owner != this) {
    if (tls.conn) {
      mysql_close(tls.conn);
      tls.conn = nullptr;
    }
    tls.conn = static_cast<MYSQL*>(Connect());
    tls.owner = tls.conn ? this : nullptr;
  }
  return tls.conn;
}

bool MysqlMetadataDao::Exec(void* conn, const std::string& sql) const {
  auto* c = static_cast<MYSQL*>(conn);
  if (!c) return false;
  if (mysql_query(c, sql.c_str()) != 0) {
    LOG_ERROR("mysql query failed: %s | sql=%s", mysql_error(c), sql.substr(0, 200).c_str());
    return false;
  }
  return true;
}

std::string MysqlMetadataDao::Escape(void* conn, const std::string& s) const {
  auto* c = static_cast<MYSQL*>(conn);
  std::string out(s.size() * 2 + 1, '\0');
  const unsigned long n = mysql_real_escape_string(c, &out[0], s.c_str(),
                                                   static_cast<unsigned long>(s.size()));
  out.resize(n);
  return out;
}

bool MysqlMetadataDao::Init() {
  void* w = WriteConn();
  if (!w) return false;
  // 逐条执行 DDL（mysql_query 一次一条）
  const std::string schema(kSchema);
  size_t pos = 0;
  while (pos < schema.size()) {
    const size_t semi = schema.find(';', pos);
    if (semi == std::string::npos) break;
    std::string stmt = schema.substr(pos, semi - pos);
    pos = semi + 1;
    // 去掉前导空白
    const size_t first = stmt.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) continue;
    stmt = stmt.substr(first);
    if (!Exec(w, stmt)) return false;
  }
  LOG_INFO("MysqlMetadataDao ready: %s:%d/%s (InnoDB, 复合索引)", cfg_.host.c_str(), cfg_.port,
           cfg_.db.c_str());
  return true;
}

bool MysqlMetadataDao::UpsertStudy(const StudyRecord& r) {
  void* w = WriteConn();
  if (!w) return false;
  const std::string sql =
      "INSERT INTO studies(study_uid,patient_id,patient_name,study_date,modality,description) VALUES('" +
      Escape(w, r.study_uid) + "','" + Escape(w, r.patient_id) + "','" + Escape(w, r.patient_name) +
      "','" + Escape(w, r.study_date) + "','" + Escape(w, r.modality) + "','" +
      Escape(w, r.description) +
      "') ON DUPLICATE KEY UPDATE patient_id=VALUES(patient_id), study_date=VALUES(study_date), "
      "modality=VALUES(modality)";
  return Exec(w, sql);
}

bool MysqlMetadataDao::UpsertSeries(const SeriesRecord& r) {
  void* w = WriteConn();
  if (!w) return false;
  const std::string sql =
      "INSERT INTO series(series_uid,study_uid,modality,series_number) VALUES('" +
      Escape(w, r.series_uid) + "','" + Escape(w, r.study_uid) + "','" + Escape(w, r.modality) +
      "'," + std::to_string(r.series_number) +
      ") ON DUPLICATE KEY UPDATE modality=VALUES(modality)";
  return Exec(w, sql);
}

bool MysqlMetadataDao::UpsertInstance(const InstanceRecord& r) {
  void* w = WriteConn();
  if (!w) return false;
  const int64_t created = r.created_at ? r.created_at : NowSeconds();
  const std::string sql =
      "INSERT INTO instances(sop_uid,series_uid,file_path,size_bytes,created_at) VALUES('" +
      Escape(w, r.sop_uid) + "','" + Escape(w, r.series_uid) + "','" + Escape(w, r.file_path) +
      "'," + std::to_string(r.size_bytes) + "," + std::to_string(created) +
      ") ON DUPLICATE KEY UPDATE file_path=VALUES(file_path)";
  return Exec(w, sql);
}

std::vector<StudyRecord> MysqlMetadataDao::QueryStudies(const QidoFilter& f) const {
  std::vector<StudyRecord> out;
  void* c = ReadConn();
  if (!c) return out;
  std::string sql =
      "SELECT study_uid,patient_id,patient_name,study_date,modality,description FROM studies WHERE 1=1";
  if (f.patient_id) sql += " AND patient_id='" + Escape(c, *f.patient_id) + "'";
  if (f.study_date) sql += " AND study_date='" + Escape(c, *f.study_date) + "'";
  if (f.modality) sql += " AND modality='" + Escape(c, *f.modality) + "'";
  sql += " ORDER BY study_date DESC LIMIT " + std::to_string(f.limit) + " OFFSET " +
         std::to_string(f.offset);

  if (mysql_query(static_cast<MYSQL*>(c), sql.c_str()) != 0) {
    LOG_ERROR("mysql query studies failed: %s", mysql_error(static_cast<MYSQL*>(c)));
    return out;
  }
  MYSQL_RES* res = mysql_store_result(static_cast<MYSQL*>(c));
  if (!res) return out;
  while (MYSQL_ROW row = mysql_fetch_row(res)) {
    StudyRecord r;
    r.study_uid = row[0] ? row[0] : "";
    r.patient_id = row[1] ? row[1] : "";
    r.patient_name = row[2] ? row[2] : "";
    r.study_date = row[3] ? row[3] : "";
    r.modality = row[4] ? row[4] : "";
    r.description = row[5] ? row[5] : "";
    out.push_back(std::move(r));
  }
  mysql_free_result(res);
  return out;
}

std::vector<SeriesRecord> MysqlMetadataDao::QuerySeries(const std::string& study_uid) const {
  std::vector<SeriesRecord> out;
  void* c = ReadConn();
  if (!c) return out;
  const std::string sql =
      "SELECT series_uid,study_uid,modality,series_number FROM series WHERE study_uid='" +
      Escape(c, study_uid) + "'";
  if (mysql_query(static_cast<MYSQL*>(c), sql.c_str()) != 0) {
    LOG_ERROR("mysql query series failed: %s", mysql_error(static_cast<MYSQL*>(c)));
    return out;
  }
  MYSQL_RES* res = mysql_store_result(static_cast<MYSQL*>(c));
  if (!res) return out;
  while (MYSQL_ROW row = mysql_fetch_row(res)) {
    SeriesRecord r;
    r.series_uid = row[0] ? row[0] : "";
    r.study_uid = row[1] ? row[1] : "";
    r.modality = row[2] ? row[2] : "";
    r.series_number = row[3] ? std::atoi(row[3]) : 0;
    out.push_back(std::move(r));
  }
  mysql_free_result(res);
  return out;
}

std::vector<InstanceRecord> MysqlMetadataDao::QueryInstances(const std::string& series_uid) const {
  std::vector<InstanceRecord> out;
  void* c = ReadConn();
  if (!c) return out;
  const std::string sql =
      "SELECT sop_uid,series_uid,file_path,size_bytes,created_at FROM instances WHERE series_uid='" +
      Escape(c, series_uid) + "'";
  if (mysql_query(static_cast<MYSQL*>(c), sql.c_str()) != 0) {
    LOG_ERROR("mysql query instances failed: %s", mysql_error(static_cast<MYSQL*>(c)));
    return out;
  }
  MYSQL_RES* res = mysql_store_result(static_cast<MYSQL*>(c));
  if (!res) return out;
  while (MYSQL_ROW row = mysql_fetch_row(res)) {
    InstanceRecord r;
    r.sop_uid = row[0] ? row[0] : "";
    r.series_uid = row[1] ? row[1] : "";
    r.file_path = row[2] ? row[2] : "";
    r.size_bytes = row[3] ? std::atoll(row[3]) : 0;
    r.created_at = row[4] ? std::atoll(row[4]) : 0;
    out.push_back(std::move(r));
  }
  mysql_free_result(res);
  return out;
}

std::vector<InstanceRecord> MysqlMetadataDao::QueryAllInstances() const {
  std::vector<InstanceRecord> out;
  void* c = ReadConn();
  if (!c) return out;
  const std::string sql = "SELECT sop_uid,series_uid,file_path,size_bytes,created_at FROM instances";
  if (mysql_query(static_cast<MYSQL*>(c), sql.c_str()) != 0) {
    LOG_ERROR("mysql query all instances failed: %s", mysql_error(static_cast<MYSQL*>(c)));
    return out;
  }
  MYSQL_RES* res = mysql_store_result(static_cast<MYSQL*>(c));
  if (!res) return out;
  while (MYSQL_ROW row = mysql_fetch_row(res)) {
    InstanceRecord r;
    r.sop_uid = row[0] ? row[0] : "";
    r.series_uid = row[1] ? row[1] : "";
    r.file_path = row[2] ? row[2] : "";
    r.size_bytes = row[3] ? std::atoll(row[3]) : 0;
    r.created_at = row[4] ? std::atoll(row[4]) : 0;
    out.push_back(std::move(r));
  }
  mysql_free_result(res);
  return out;
}

std::optional<InstanceRecord> MysqlMetadataDao::FindInstance(const std::string& sop_uid) const {
  void* c = ReadConn();
  if (!c) return std::nullopt;
  const std::string sql =
      "SELECT sop_uid,series_uid,file_path,size_bytes,created_at FROM instances WHERE sop_uid='" +
      Escape(c, sop_uid) + "' LIMIT 1";
  if (mysql_query(static_cast<MYSQL*>(c), sql.c_str()) != 0) {
    LOG_ERROR("mysql find instance failed: %s", mysql_error(static_cast<MYSQL*>(c)));
    return std::nullopt;
  }
  MYSQL_RES* res = mysql_store_result(static_cast<MYSQL*>(c));
  if (!res) return std::nullopt;
  std::optional<InstanceRecord> out;
  if (MYSQL_ROW row = mysql_fetch_row(res)) {
    InstanceRecord r;
    r.sop_uid = row[0] ? row[0] : "";
    r.series_uid = row[1] ? row[1] : "";
    r.file_path = row[2] ? row[2] : "";
    r.size_bytes = row[3] ? std::atoll(row[3]) : 0;
    r.created_at = row[4] ? std::atoll(row[4]) : 0;
    out = std::move(r);
  }
  mysql_free_result(res);
  return out;
}

bool MysqlMetadataDao::Ping() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!conn_) {
    conn_ = Connect();
    return conn_ != nullptr;
  }
  if (mysql_ping(static_cast<MYSQL*>(conn_)) == 0) return true;
  // mysql_ping 不会自动重连：断开后重连一次，MySQL 恢复后服务能自愈
  mysql_close(static_cast<MYSQL*>(conn_));
  conn_ = Connect();
  return conn_ != nullptr;
}

bool MysqlMetadataDao::VerifyConsistency() const {
  auto all = QueryAllInstances();
  size_t missing = 0;
  for (const auto& r : all) {
    struct stat st {};
    if (::stat(r.file_path.c_str(), &st) != 0) ++missing;
  }
  if (missing) {
    LOG_WARN("VerifyConsistency: %zu/%zu object(s) missing on disk", missing, all.size());
  } else {
    LOG_INFO("VerifyConsistency: %zu instance(s) all present", all.size());
  }
  return missing == 0;
}

#endif  // DWS_HAVE_MYSQL

}  // namespace dws
