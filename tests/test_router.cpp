// 路由 + DICOMweb 闭环单测：STOW 上传 -> 落盘 + 元数据 -> QIDO 检索 -> WADO 下载。
#include "api/Router.h"
#include "store/MetadataDao.h"
#include "store/ObjectStore.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace dws;

namespace {
std::string Root() { return "/tmp/dws_router_" + std::to_string(::getpid()); }
void RmTree(const std::string& root) {
  std::string cmd = "rm -rf '" + root + "'";
  if (std::system(cmd.c_str()) != 0) {
    // 测试清理失败不影响断言
  }
}
// 建好干净的测试目录（SQLite 不会自动创建父目录）
void Prepare(const std::string& root) {
  RmTree(root);
  std::string cmd = "mkdir -p '" + root + "'";
  if (std::system(cmd.c_str()) != 0) {
    // 创建失败会在后续断言中暴露
  }
}
}  // namespace

TEST(Router, StowThenQueryThenWado) {
  const std::string root = Root();
  Prepare(root);

  ObjectStore store(root + "/objects");
  MetadataDao dao(root + "/meta.db");
  ASSERT_TRUE(dao.Init());

  Router router;
  ApiContext ctx;
  ctx.store = &store;
  ctx.dao = &dao;
  router.set_context(ctx);
  router.RegisterDicomweb();

  // STOW：multipart 上传 1 个实例
  const std::string boundary = "BOUND";
  std::string dicom(256, '\0');
  dicom[128] = 'D';
  dicom[129] = 'I';
  dicom[130] = 'C';
  dicom[131] = 'M';

  HttpRequest up;
  up.method = "POST";
  up.path = "/studies";
  up.content_type = "multipart/related; boundary=" + boundary;
  up.boundary = boundary;
  MultipartPart part;
  part.body = dicom;
  up.multipart_parts.push_back(part);

  HttpResponse r1 = router.Dispatch(up);
  EXPECT_EQ(r1.status, 200);
  EXPECT_NE(r1.body.find("\"ok\":1"), std::string::npos);

  // 对象已落盘 + 元数据已写入
  auto all = dao.QueryAllInstances();
  ASSERT_EQ(all.size(), 1u);
  EXPECT_TRUE(store.Exists(all[0].sop_uid));

  // QIDO：检索到该 study（返回 DICOM JSON 里含 StudyInstanceUID 标签 0020000D）
  HttpRequest q;
  q.method = "GET";
  q.path = "/studies";
  HttpResponse r2 = router.Dispatch(q);
  EXPECT_EQ(r2.status, 200);
  EXPECT_NE(r2.body.find("0020000D"), std::string::npos);

  // WADO：按 series 下载 -> multipart/related
  HttpRequest w;
  w.method = "GET";
  w.path = "/studies/x/series/" + all[0].series_uid;
  HttpResponse r3 = router.Dispatch(w);
  EXPECT_EQ(r3.status, 200);
  EXPECT_TRUE(r3.is_multipart());
  ASSERT_EQ(r3.parts.size(), 1u);
  EXPECT_EQ(r3.parts[0].file_path, all[0].file_path);

  // WADO：按实例下载（默认返回 multipart/related，符合 PS3.18）
  HttpRequest wi;
  wi.method = "GET";
  wi.path = "/studies/x/series/" + all[0].series_uid + "/instances/" + all[0].sop_uid;
  HttpResponse r4 = router.Dispatch(wi);
  EXPECT_EQ(r4.status, 200);
  EXPECT_TRUE(r4.is_multipart());

  // WADO：客户端明确要求单体 DICOM 时返回文件
  HttpRequest wi2 = wi;
  wi2.headers["accept"] = "application/dicom";
  HttpResponse r5 = router.Dispatch(wi2);
  EXPECT_EQ(r5.status, 200);
  EXPECT_TRUE(r5.is_file());

  RmTree(root);
}

TEST(Router, NotFoundReturns404) {
  const std::string root = Root() + "_nf";
  Prepare(root);
  ObjectStore store(root + "/objects");
  MetadataDao dao(root + "/meta.db");
  ASSERT_TRUE(dao.Init());

  Router router;
  ApiContext ctx;
  ctx.store = &store;
  ctx.dao = &dao;
  router.set_context(ctx);
  router.RegisterDicomweb();

  HttpRequest w;
  w.method = "GET";
  w.path = "/studies/a/series/nonexistent/instances/i";
  HttpResponse r = router.Dispatch(w);
  EXPECT_EQ(r.status, 404);
}
