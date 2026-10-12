-- 元数据库表结构与索引（SQLite；PostgreSQL 版去掉 IF NOT EXISTS / 改类型即可）
-- 关键：WAL 保证断电一致性；复合索引 (patient_id, study_date, modality) 服务 QIDO 检索。

PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;

CREATE TABLE IF NOT EXISTS studies (
  study_uid    TEXT PRIMARY KEY,
  patient_id   TEXT NOT NULL,
  patient_name TEXT,
  study_date   TEXT,
  modality     TEXT,
  description  TEXT
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

-- QIDO-RS 检索主路径（最左前缀：patient_id 必选，study_date/modality 可选）
CREATE INDEX IF NOT EXISTS idx_study_lookup ON studies(patient_id, study_date, modality);
CREATE INDEX IF NOT EXISTS idx_series_study ON series(study_uid);
CREATE INDEX IF NOT EXISTS idx_inst_series  ON instances(series_uid);

-- 验收辅助：确认索引命中（应出现 USING INDEX idx_study_lookup）
-- EXPLAIN QUERY PLAN
-- SELECT * FROM studies WHERE patient_id='P001' AND study_date='20260101' LIMIT 100;
