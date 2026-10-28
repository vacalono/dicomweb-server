#pragma once
// 元数据记录与查询过滤条件：DAO 实现（SQLite / MySQL）共用，不绑定任何数据库驱动。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

}  // namespace dws
