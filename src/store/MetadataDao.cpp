#include "store/MetadataDao.h"

#include "util/Logger.h"

#include <sys/stat.h>

#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(DWS_HAVE_SQLITE3)
#include <sqlite3.h>
#endif

namespace dws {

namespace {
const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS studies (
  study_uid   TEXT PRIMARY KEY,
  patient_id  TEXT NOT NULL,
  patient_name TEXT,
  study_date  TEXT,
  modality    TEXT,
  description TEXT
);
CREATE TABLE IF NOT EXISTS series (
  series_uid    TEXT PRIMARY KEY,
  study_uid     TEXT NOT NULL REFERENCES studies(study_uid) ON DELETE CASCADE,
  modality      TEXT,
  series_number INTEGER
);
CREATE TABLE IF NOT EXISTS instances (
  sop_uid    TEXT PRIMARY KEY,
  series_uid TEXT NOT NULL REFERENCES series(series_uid) ON DELETE CASCADE,
  file_path  TEXT NOT NULL,
  size_bytes INTEGER,
  created_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_study_lookup  ON studies(patient_id, study_date, modality);
CREATE INDEX IF NOT EXISTS idx_series_study  ON series(study_uid);
CREATE INDEX IF NOT EXISTS idx_inst_series   ON instances(series_uid);
)SQL";

int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}  // namespace

MetadataDao::MetadataDao(const std::string& db_path) : db_path_(db_path) {}

MetadataDao::~MetadataDao() {
#if defined(DWS_HAVE_SQLITE3)
  if (db_) sqlite3_close(db_);
#endif
}

bool MetadataDao::Init() {
#if !defined(DWS_HAVE_SQLITE3)
  LOG_WARN("MetadataDao compiled without SQLite3 -> operations are no-ops");
  return true;
#else
  if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
    LOG_ERROR("sqlite3_open(%s) failed: %s", db_path_.c_str(), sqlite3_errmsg(db_));
    return false;
  }
  // WAL：断电一致性 + 读写并发
  char* err = nullptr;
  sqlite3_exec(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;", nullptr, nullptr, &err);
  if (err) {
    LOG_WARN("pragma: %s", err);
    sqlite3_free(err);
    err = nullptr;
  }
  if (sqlite3_exec(db_, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
    LOG_ERROR("schema init failed: %s", err ? err : "?");
    sqlite3_free(err);
    return false;
  }
  LOG_INFO("MetadataDao ready: %s (WAL)", db_path_.c_str());
  return true;
#endif
}

#if defined(DWS_HAVE_SQLITE3)
// 每线程一个独立只读连接（NOMUTEX：该连接仅被本线程使用），
// 避免多 worker 并发读时争抢同一连接的 serialized 锁（实测会把吞吐拖垮 8x）。
sqlite3* MetadataDao::ReadConn() const {
  struct Tls {
    sqlite3* conn = nullptr;
    const MetadataDao* owner = nullptr;
  };
  static thread_local Tls tls;
  if (tls.conn == nullptr || tls.owner != this) {
    if (tls.conn) {
      sqlite3_close(tls.conn);
      tls.conn = nullptr;
    }
    if (sqlite3_open_v2(db_path_.c_str(), &tls.conn,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) == SQLITE_OK) {
      tls.owner = this;
    } else {
      if (tls.conn) {
        sqlite3_close(tls.conn);
        tls.conn = nullptr;
      }
      tls.owner = nullptr;
    }
  }
  return tls.conn;
}
#endif

#if defined(DWS_HAVE_SQLITE3)
namespace {

bool ExecBind(sqlite3* db, const char* sql,
              const std::vector<std::pair<int, std::string>>& params,
              const std::vector<std::pair<int, int64_t>>& ints = {}) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
  for (const auto& p : params) sqlite3_bind_text(stmt, p.first, p.second.c_str(), -1, SQLITE_TRANSIENT);
  for (const auto& p : ints) sqlite3_bind_int64(stmt, p.first, p.second);
  bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

}  // namespace
#endif

bool MetadataDao::UpsertStudy(const StudyRecord& r) {
#if !defined(DWS_HAVE_SQLITE3)
  (void)r; return true;
#else
  const char* sql =
      "INSERT INTO studies(study_uid,patient_id,patient_name,study_date,modality,description) "
      "VALUES(?,?,?,?,?,?) ON CONFLICT(study_uid) DO UPDATE SET patient_id=excluded.patient_id, "
      "study_date=excluded.study_date, modality=excluded.modality";
  return ExecBind(db_, sql,
                  {{1, r.study_uid}, {2, r.patient_id}, {3, r.patient_name},
                   {4, r.study_date}, {5, r.modality}, {6, r.description}});
#endif
}

bool MetadataDao::UpsertSeries(const SeriesRecord& r) {
#if !defined(DWS_HAVE_SQLITE3)
  (void)r; return true;
#else
  const char* sql =
      "INSERT INTO series(series_uid,study_uid,modality,series_number) VALUES(?,?,?,?) "
      "ON CONFLICT(series_uid) DO UPDATE SET modality=excluded.modality";
  return ExecBind(db_, sql,
                  {{1, r.series_uid}, {2, r.study_uid}, {3, r.modality}},
                  {{4, r.series_number}});
#endif
}

bool MetadataDao::UpsertInstance(const InstanceRecord& r) {
#if !defined(DWS_HAVE_SQLITE3)
  (void)r; return true;
#else
  const char* sql =
      "INSERT INTO instances(sop_uid,series_uid,file_path,size_bytes,created_at) VALUES(?,?,?,?,?) "
      "ON CONFLICT(sop_uid) DO UPDATE SET file_path=excluded.file_path";
  int64_t created = r.created_at ? r.created_at : NowSeconds();
  return ExecBind(db_, sql,
                  {{1, r.sop_uid}, {2, r.series_uid}, {3, r.file_path}},
                  {{4, r.size_bytes}, {5, created}});
#endif
}

std::vector<StudyRecord> MetadataDao::QueryStudies(const QidoFilter& f) const {
  std::vector<StudyRecord> out;
#if defined(DWS_HAVE_SQLITE3)
  std::string sql =
      "SELECT study_uid,patient_id,patient_name,study_date,modality,description FROM studies WHERE 1=1";
  if (f.patient_id) sql += " AND patient_id=?";
  if (f.study_date) sql += " AND study_date=?";
  if (f.modality) sql += " AND modality=?";
  sql += " ORDER BY study_date DESC LIMIT ? OFFSET ?";

  sqlite3* conn = ReadConn();
  if (!conn) return out;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(conn, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return out;
  int idx = 1;
  if (f.patient_id) sqlite3_bind_text(stmt, idx++, f.patient_id->c_str(), -1, SQLITE_TRANSIENT);
  if (f.study_date) sqlite3_bind_text(stmt, idx++, f.study_date->c_str(), -1, SQLITE_TRANSIENT);
  if (f.modality) sqlite3_bind_text(stmt, idx++, f.modality->c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, idx++, f.limit);
  sqlite3_bind_int(stmt, idx++, f.offset);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    StudyRecord r;
    auto col = [&](int i) {
      const unsigned char* t = sqlite3_column_text(stmt, i);
      return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
    };
    r.study_uid = col(0);
    r.patient_id = col(1);
    r.patient_name = col(2);
    r.study_date = col(3);
    r.modality = col(4);
    r.description = col(5);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(stmt);
#else
  (void)f;
#endif
  return out;
}

std::vector<SeriesRecord> MetadataDao::QuerySeries(const std::string& study_uid) const {
  std::vector<SeriesRecord> out;
#if defined(DWS_HAVE_SQLITE3)
  const char* sql = "SELECT series_uid,study_uid,modality,series_number FROM series WHERE study_uid=?";
  sqlite3* conn = ReadConn();
  if (!conn) return out;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(conn, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(stmt, 1, study_uid.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    SeriesRecord r;
    const unsigned char* a = sqlite3_column_text(stmt, 0);
    const unsigned char* b = sqlite3_column_text(stmt, 1);
    const unsigned char* c = sqlite3_column_text(stmt, 2);
    r.series_uid = a ? reinterpret_cast<const char*>(a) : "";
    r.study_uid = b ? reinterpret_cast<const char*>(b) : "";
    r.modality = c ? reinterpret_cast<const char*>(c) : "";
    r.series_number = sqlite3_column_int(stmt, 3);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(stmt);
#else
  (void)study_uid;
#endif
  return out;
}

std::vector<InstanceRecord> MetadataDao::QueryInstances(const std::string& series_uid) const {
  std::vector<InstanceRecord> out;
#if defined(DWS_HAVE_SQLITE3)
  const char* sql =
      "SELECT sop_uid,series_uid,file_path,size_bytes,created_at FROM instances WHERE series_uid=?";
  sqlite3* conn = ReadConn();
  if (!conn) return out;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(conn, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(stmt, 1, series_uid.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    InstanceRecord r;
    const unsigned char* a = sqlite3_column_text(stmt, 0);
    const unsigned char* b = sqlite3_column_text(stmt, 1);
    const unsigned char* c = sqlite3_column_text(stmt, 2);
    r.sop_uid = a ? reinterpret_cast<const char*>(a) : "";
    r.series_uid = b ? reinterpret_cast<const char*>(b) : "";
    r.file_path = c ? reinterpret_cast<const char*>(c) : "";
    r.size_bytes = sqlite3_column_int64(stmt, 3);
    r.created_at = sqlite3_column_int64(stmt, 4);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(stmt);
#else
  (void)series_uid;
#endif
  return out;
}

std::vector<InstanceRecord> MetadataDao::QueryAllInstances() const {
  std::vector<InstanceRecord> out;
#if defined(DWS_HAVE_SQLITE3)
  const char* sql = "SELECT sop_uid,series_uid,file_path,size_bytes,created_at FROM instances";
  sqlite3* conn = ReadConn();
  if (!conn) return out;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(conn, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    InstanceRecord r;
    const unsigned char* a = sqlite3_column_text(stmt, 0);
    const unsigned char* b = sqlite3_column_text(stmt, 1);
    const unsigned char* c = sqlite3_column_text(stmt, 2);
    r.sop_uid = a ? reinterpret_cast<const char*>(a) : "";
    r.series_uid = b ? reinterpret_cast<const char*>(b) : "";
    r.file_path = c ? reinterpret_cast<const char*>(c) : "";
    r.size_bytes = sqlite3_column_int64(stmt, 3);
    r.created_at = sqlite3_column_int64(stmt, 4);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(stmt);
#endif
  return out;
}

std::optional<InstanceRecord> MetadataDao::FindInstance(const std::string& sop_uid) const {
#if defined(DWS_HAVE_SQLITE3)
  const char* sql = "SELECT sop_uid,series_uid,file_path,size_bytes,created_at FROM instances WHERE sop_uid=?";
  sqlite3* conn = ReadConn();
  if (!conn) return std::nullopt;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(conn, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
  sqlite3_bind_text(stmt, 1, sop_uid.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<InstanceRecord> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    InstanceRecord r;
    const unsigned char* a = sqlite3_column_text(stmt, 0);
    const unsigned char* b = sqlite3_column_text(stmt, 1);
    const unsigned char* c = sqlite3_column_text(stmt, 2);
    r.sop_uid = a ? reinterpret_cast<const char*>(a) : "";
    r.series_uid = b ? reinterpret_cast<const char*>(b) : "";
    r.file_path = c ? reinterpret_cast<const char*>(c) : "";
    r.size_bytes = sqlite3_column_int64(stmt, 3);
    r.created_at = sqlite3_column_int64(stmt, 4);
    out = std::move(r);
  }
  sqlite3_finalize(stmt);
  return out;
#else
  (void)sop_uid;
  return std::nullopt;
#endif
}

bool MetadataDao::Ping() {
#if defined(DWS_HAVE_SQLITE3)
  sqlite3* conn = ReadConn();
  if (!conn) return false;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(conn, "SELECT 1", -1, &stmt, nullptr) != SQLITE_OK) return false;
  const bool ok = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return ok;
#else
  return false;
#endif
}

bool MetadataDao::VerifyConsistency() const {
  // 遍历所有实例元数据，核对对应对象文件是否真实存在。
  // 断电/崩溃后可能残留"有元数据无对象"或反之，据此定位需要修复的记录。
  auto all = QueryAllInstances();
  size_t missing = 0;
  for (const auto& r : all) {
    struct stat st{};
    if (::stat(r.file_path.c_str(), &st) != 0) {
      ++missing;
      LOG_WARN("inconsistent: object missing (sop=%s path=%s)", r.sop_uid.c_str(),
               r.file_path.c_str());
    }
  }
  if (missing) {
    LOG_WARN("VerifyConsistency: %zu/%zu object(s) missing on disk", missing, all.size());
  } else {
    LOG_INFO("VerifyConsistency: %zu instance(s) all present", all.size());
  }
  return missing == 0;
}

}  // namespace dws
