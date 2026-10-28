// 独立的 gRPC 元数据服务（P5 对照用）：只提供 ListStudies，
// 与 HTTP/JSON 的 GET /studies 查同一张表，用来对比协议与序列化开销。
#include <grpcpp/grpcpp.h>

#include <cstdio>
#include <memory>
#include <string>

#include "metadata.grpc.pb.h"
#include "store/DaoTypes.h"
#include "store/IMetadataDao.h"
#include "util/Logger.h"

namespace {

class MetadataServiceImpl final : public dws::rpc::Metadata::Service {
 public:
  explicit MetadataServiceImpl(dws::IMetadataDao* dao) : dao_(dao) {}

  grpc::Status ListStudies(grpc::ServerContext*, const dws::rpc::StudyFilter* req,
                           dws::rpc::StudyList* resp) override {
    dws::QidoFilter f;
    if (!req->patient_id().empty()) f.patient_id = req->patient_id();
    if (!req->study_date().empty()) f.study_date = req->study_date();
    if (!req->modality().empty()) f.modality = req->modality();
    f.limit = req->limit() > 0 ? req->limit() : 100;
    f.offset = req->offset() > 0 ? req->offset() : 0;

    for (const auto& s : dao_->QueryStudies(f)) {
      auto* out = resp->add_studies();
      out->set_study_uid(s.study_uid);
      out->set_patient_id(s.patient_id);
      out->set_patient_name(s.patient_name);
      out->set_study_date(s.study_date);
      out->set_modality(s.modality);
    }
    return grpc::Status::OK;
  }

 private:
  dws::IMetadataDao* dao_;
};

}  // namespace

int main(int argc, char** argv) {
  std::string addr = "0.0.0.0:50051";
  std::string db_path = "./data/meta.db";
  std::string backend = "sqlite";
  std::string mysql_dsn;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* d) { return i + 1 < argc ? std::string(argv[++i]) : std::string(d); };
    if (a == "--addr") addr = next("0.0.0.0:50051");
    else if (a == "--db") db_path = next("./data/meta.db");
    else if (a == "--db-backend") backend = next("sqlite");
    else if (a == "--mysql-dsn") mysql_dsn = next("");
    else if (a == "--help" || a == "-h") {
      std::printf("usage: %s [--addr host:port] [--db path] [--db-backend sqlite|mysql] "
                  "[--mysql-dsn dsn]\n", argv[0]);
      return 0;
    }
  }

  auto dao = dws::CreateMetadataDao(backend, backend == "mysql" ? mysql_dsn : db_path);
  if (!dao || !dao->Init()) {
    LOG_ERROR("metadata init failed");
    return 1;
  }

  MetadataServiceImpl service(dao.get());
  grpc::ServerBuilder builder;
  builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  if (!server) {
    LOG_ERROR("grpc server failed to start on %s", addr.c_str());
    return 1;
  }
  LOG_INFO("grpc metadata service listening on %s (backend=%s)", addr.c_str(), dao->backend());
  server->Wait();
  return 0;
}
