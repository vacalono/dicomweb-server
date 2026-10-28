// DAO 契约测试：同一套断言分别跑 SQLite 与 MySQL，保证两个后端语义一致。
// MySQL 通过环境变量 DWS_TEST_MYSQL_DSN 开启（CI 里指向 service container）。
#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "store/IMetadataDao.h"

namespace {

std::string Uniq(const char* tag) {
  static int n = 0;
  return std::string("1.2.826.0.1.3680043.10.9999.") + tag + "." +
         std::to_string(static_cast<long>(::getpid())) + "." + std::to_string(++n);
}

// 契约：三层 Upsert → 查询 → 过滤 → 幂等 → 一致性
void RunContract(dws::IMetadataDao* dao) {
  const std::string study = Uniq("study");
  const std::string series = Uniq("series");
  const std::string sop = Uniq("sop");
  const std::string pid = "P" + std::to_string(::getpid());

  // 临时对象文件（VerifyConsistency 要求 file_path 真实存在）
  const auto tmp = std::filesystem::temp_directory_path() / ("dws_dao_" + sop + ".dcm");
  { std::ofstream f(tmp); f << "DICM"; }

  dws::StudyRecord s;
  s.study_uid = study;
  s.patient_id = pid;
  s.patient_name = "Test^Patient";
  s.study_date = "20261028";
  s.modality = "CT";
  s.description = "contract";
  ASSERT_TRUE(dao->UpsertStudy(s));

  dws::SeriesRecord se;
  se.series_uid = series;
  se.study_uid = study;
  se.modality = "CT";
  se.series_number = 1;
  ASSERT_TRUE(dao->UpsertSeries(se));

  dws::InstanceRecord in;
  in.sop_uid = sop;
  in.series_uid = series;
  in.file_path = tmp.string();
  in.size_bytes = 4;
  ASSERT_TRUE(dao->UpsertInstance(in));

  // 幂等：重复写不报错、不产生重复行
  ASSERT_TRUE(dao->UpsertStudy(s));
  ASSERT_TRUE(dao->UpsertInstance(in));

  // 按 PatientID 过滤
  dws::QidoFilter f;
  f.patient_id = pid;
  auto studies = dao->QueryStudies(f);
  ASSERT_GE(studies.size(), 1u);
  bool found = false;
  for (const auto& r : studies) {
    if (r.study_uid == study) {
      found = true;
      EXPECT_EQ(r.modality, "CT");
      EXPECT_EQ(r.study_date, "20261028");
    }
  }
  EXPECT_TRUE(found) << "QueryStudies 没返回刚写入的 study";

  auto series_list = dao->QuerySeries(study);
  ASSERT_EQ(series_list.size(), 1u);
  EXPECT_EQ(series_list[0].series_uid, series);

  auto inst = dao->QueryInstances(series);
  ASSERT_EQ(inst.size(), 1u);
  EXPECT_EQ(inst[0].sop_uid, sop);

  auto one = dao->FindInstance(sop);
  ASSERT_TRUE(one.has_value());
  EXPECT_EQ(one->series_uid, series);

  // 一致性：对象文件存在 → true
  EXPECT_TRUE(dao->VerifyConsistency());

  std::filesystem::remove(tmp);
}

}  // namespace

TEST(DaoContract, SqliteBackend) {
  const auto db = std::filesystem::temp_directory_path() / "dws_contract.sqlite";
  std::filesystem::remove(db);
  auto dao = dws::CreateMetadataDao("sqlite", db.string());
  ASSERT_NE(dao, nullptr);
  EXPECT_STREQ(dao->backend(), "sqlite");
  ASSERT_TRUE(dao->Init());
  RunContract(dao.get());
}

TEST(DaoContract, MysqlBackend) {
  const char* dsn = std::getenv("DWS_TEST_MYSQL_DSN");
  if (!dsn || !*dsn) {
    GTEST_SKIP() << "DWS_TEST_MYSQL_DSN 未设置，跳过 MySQL 契约测试";
  }
  auto dao = dws::CreateMetadataDao("mysql", dsn);
  ASSERT_NE(dao, nullptr);
  EXPECT_STREQ(dao->backend(), "mysql");
  ASSERT_TRUE(dao->Init()) << "MySQL 不可用时这个用例应当失败（CI 里必须真连）";
  RunContract(dao.get());
}

TEST(DaoContract, UnknownBackendReturnsNull) {
  EXPECT_EQ(dws::CreateMetadataDao("postgres", "x"), nullptr);
}
