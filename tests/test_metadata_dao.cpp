// W3 验收：元数据写入/检索 + 索引生效。
#include "store/MetadataDao.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <string>

using namespace dws;

namespace {
std::string TempDb() { return "/tmp/dws_test_" + std::to_string(::getpid()) + ".db"; }
}  // namespace

TEST(MetadataDao, UpsertAndQuery) {
  const std::string db = TempDb();
  {
    MetadataDao dao(db);
    ASSERT_TRUE(dao.Init());

    StudyRecord s;
    s.study_uid = "1.2.3";
    s.patient_id = "P001";
    s.study_date = "20260101";
    s.modality = "CT";
    s.description = "chest";
    EXPECT_TRUE(dao.UpsertStudy(s));

    SeriesRecord se;
    se.series_uid = "1.2.3.1";
    se.study_uid = "1.2.3";
    se.modality = "CT";
    se.series_number = 1;
    EXPECT_TRUE(dao.UpsertSeries(se));

    InstanceRecord in;
    in.sop_uid = "1.2.3.1.1";
    in.series_uid = "1.2.3.1";
    in.file_path = "/tmp/x.dcm";
    in.size_bytes = 1234;
    EXPECT_TRUE(dao.UpsertInstance(in));

    QidoFilter f;
    f.patient_id = "P001";
    auto studies = dao.QueryStudies(f);
    ASSERT_EQ(studies.size(), 1u);
    EXPECT_EQ(studies[0].study_uid, "1.2.3");

    auto series = dao.QuerySeries("1.2.3");
    ASSERT_EQ(series.size(), 1u);

    auto inst = dao.QueryInstances("1.2.3.1");
    ASSERT_EQ(inst.size(), 1u);
    EXPECT_EQ(inst[0].file_path, "/tmp/x.dcm");

    auto one = dao.FindInstance("1.2.3.1.1");
    ASSERT_TRUE(one.has_value());
  }
  std::remove(db.c_str());
  std::remove((db + "-wal").c_str());
  std::remove((db + "-shm").c_str());
}

// W4：元数据指向的对象文件缺失时，一致性校验应失败
TEST(MetadataDao, VerifyConsistencyDetectsMissingObject) {
  const std::string db = TempDb();
  MetadataDao dao(db);
  ASSERT_TRUE(dao.Init());

  InstanceRecord in;
  in.sop_uid = "1.2.3.9.9";
  in.series_uid = "1.2.3.9";
  in.file_path = "/tmp/dws_definitely_missing_9_9.dcm";
  in.size_bytes = 10;
  ASSERT_TRUE(dao.UpsertInstance(in));

  EXPECT_FALSE(dao.VerifyConsistency());

  std::remove(db.c_str());
  std::remove((db + "-wal").c_str());
  std::remove((db + "-shm").c_str());
}
