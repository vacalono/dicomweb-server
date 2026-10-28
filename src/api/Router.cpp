#include "api/Router.h"

#include "dicom/DicomFile.h"
#include "store/IMetadataDao.h"
#include "store/MetadataDao.h"
#include "store/QidoCache.h"
#include "util/HealthGuard.h"
#include "util/RateLimiter.h"
#include "store/ObjectStore.h"
#include "util/Logger.h"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dws {

namespace {

// QIDO 缓存键：qido:v1:{kind}:{uid?}:{sorted query}
std::string QidoCacheKey(const HttpRequest& req, const std::vector<std::string>& args) {
  std::string k = "qido:v1:";
  if (args.empty()) {
    k += "studies:";
  } else if (args.size() == 1) {
    k += "series:" + args[0] + ":";
  } else {
    k += "inst:" + args[1] + ":";
  }
  std::vector<std::pair<std::string, std::string>> kv(req.query.begin(), req.query.end());
  std::sort(kv.begin(), kv.end());
  for (const auto& p : kv) {
    k += p.first;
    k += '=';
    k += p.second;
    k += '&';
  }
  return k;
}

std::vector<std::string> SplitPath(const std::string& path) {
  std::vector<std::string> segs;
  size_t pos = 0;
  while (pos < path.size()) {
    while (pos < path.size() && path[pos] == '/') ++pos;
    size_t next = path.find('/', pos);
    if (next == std::string::npos) next = path.size();
    if (next > pos) segs.push_back(path.substr(pos, next - pos));
    pos = next;
  }
  return segs;
}

std::string JsonEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default: o += c;
    }
  }
  return o;
}

// 追加式转义：直接写入目标 buffer，避免每次构造临时 string（大结果集下减少分配/拷贝）
void JsonEscapeInto(std::string& o, const std::string& s) {
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default: o += c;
    }
  }
}

// multipart 切分已上移到 HttpParser（解析器负责把 body 按 boundary 切成 parts）
}  // namespace

void Router::Add(const std::string& method, const std::string& pattern, Handler h) {
  Route r;
  r.method = method;
  r.segs = SplitPath(pattern);
  r.handler = std::move(h);
  routes_.push_back(std::move(r));
}

void Router::set_rate_limit(int rps) {
  if (rps <= 0) {
    limiter_.reset();
    return;
  }
  const double burst = std::max(1.0, rps / 10.0);
  limiter_ = std::make_unique<TokenBucket>(static_cast<double>(rps), burst);
  LOG_INFO("rate limit: %d rps (burst %.0f)", rps, burst);
}

// 指标与限流的外壳：所有请求都经过这里
HttpResponse Router::Dispatch(const HttpRequest& req) {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  metrics_.IncRequests();

  HttpResponse resp;
  if (limiter_ && !limiter_->TryAcquire()) {
    metrics_.IncRateLimited();
    resp = HttpResponse::Error(429, "rate limited");
    resp.headers["retry-after"] = "1";
  } else {
    resp = DispatchInner(req);
    if (resp.status >= 500) metrics_.IncErrors();
  }

  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0).count();
  metrics_.ObserveUs(static_cast<uint64_t>(us < 0 ? 0 : us));
  return resp;
}

HttpResponse Router::DispatchInner(const HttpRequest& req) {
  // 熔断打开 → 数据库相关请求快速失败（/metrics 本身不依赖 DB，放行）
  if (guard_ && req.path != "/metrics" && !guard_->db_up()) {
    HttpResponse r = HttpResponse::Error(503, "database unavailable (circuit open)");
    r.headers["retry-after"] = "2";
    return r;
  }
  auto req_segs = SplitPath(req.path);
  for (const auto& r : routes_) {
    if (r.method != req.method) continue;
    if (r.segs.size() != req_segs.size()) continue;
    Args captures;
    bool ok = true;
    for (size_t i = 0; i < r.segs.size(); ++i) {
      const std::string& p = r.segs[i];
      if (p.size() > 1 && p[0] == ':') {
        captures.push_back(req_segs[i]);
      } else if (p != req_segs[i]) {
        ok = false;
        break;
      }
    }
    if (ok) return r.handler(req, captures);
  }
  LOG_WARN("no route: method=%s path=[%s] path_len=%zu cl=%lld", req.method.c_str(),
           req.path.c_str(), req.path.size(),
           static_cast<long long>(req.content_length.value_or(0)));
  return HttpResponse::Error(404, "no route");
}

void Router::RegisterDicomweb() {
  // 指标端点（Prometheus 文本）：不加限流之外的鉴权，内网/本地使用
  Get("/metrics", [this](const HttpRequest&, const Args&) {
    HttpResponse r;
    r.content_type = "text/plain; version=0.0.4; charset=utf-8";
    r.kind = HttpResponse::BodyKind::kString;
    r.body = metrics_.RenderPrometheus();
    if (guard_) {
      r.body += "# HELP dicomweb_db_up metadata DB reachable (1/0)\n";
      r.body += "# TYPE dicomweb_db_up gauge\n";
      r.body += "dicomweb_db_up " + std::to_string(guard_->db_up() ? 1 : 0) + "\n";
      r.body += "# HELP dicomweb_cache_up redis cache reachable (1/0)\n";
      r.body += "# TYPE dicomweb_cache_up gauge\n";
      r.body += "dicomweb_cache_up " + std::to_string(guard_->cache_up() ? 1 : 0) + "\n";
      r.body += "dicomweb_db_breaker{failures=\"" + std::to_string(guard_->db_breaker().failures()) +
                "\",state=\"" + guard_->db_breaker().state_name() + "\"} 1\n";
      r.body += "dicomweb_cache_breaker{failures=\"" +
                std::to_string(guard_->cache_breaker().failures()) + "\",state=\"" +
                guard_->cache_breaker().state_name() + "\"} 1\n";
    }
    return r;
  });

  // STOW-RS：上传
  Post("/studies", [this](const HttpRequest& q, const Args& a) { return HandleStow(q, a); });
  Post("/studies/:study", [this](const HttpRequest& q, const Args& a) { return HandleStow(q, a); });
  // QIDO-RS：检索
  Get("/studies", [this](const HttpRequest& q, const Args& a) { return HandleQido(q, a); });
  Get("/studies/:study", [this](const HttpRequest& q, const Args& a) { return HandleQido(q, a); });
  Get("/studies/:study/series", [this](const HttpRequest& q, const Args& a) { return HandleQido(q, a); });
  Get("/studies/:study/series/:series/instances",
      [this](const HttpRequest& q, const Args& a) { return HandleQido(q, a); });
  // WADO-RS：下载
  Get("/studies/:study/series/:series",
      [this](const HttpRequest& q, const Args& a) { return HandleWado(q, a); });
  Get("/studies/:study/series/:series/instances/:instance",
      [this](const HttpRequest& q, const Args& a) { return HandleWado(q, a); });
}

HttpResponse Router::HandleQido(const HttpRequest& req, const Args& args) const {
  if (!ctx_.dao) return HttpResponse::Error(500, "dao not configured");

  // 命中缓存则直接返回（TTL 60s，STOW 后按 study/series 前缀失效）
  const bool cache_ok = ctx_.cache && (!guard_ || guard_->cache_up());
  const std::string cache_key = cache_ok ? QidoCacheKey(req, args) : std::string();
  if (cache_ok) {
    std::string hit;
    if (ctx_.cache->Get(cache_key, &hit)) {
      metrics_.IncCacheHit();
      HttpResponse r = HttpResponse::Json(hit);
      r.headers["x-cache"] = "HIT";
      return r;
    }
    metrics_.IncCacheMiss();
  }

  QidoFilter f;
  auto get = [&req](const char* k) -> std::optional<std::string> {
    auto it = req.query.find(k);
    if (it == req.query.end() || it->second.empty()) return std::nullopt;
    return it->second;
  };
  f.patient_id = get("PatientID");
  f.study_date = get("StudyDate");
  f.modality = get("Modality");
  if (auto l = get("limit")) f.limit = std::atoi(l->c_str());
  if (auto o = get("offset")) f.offset = std::atoi(o->c_str());

  // 直接用 string 拼接（避免 ostringstream 的 locale/虚函数开销 —— 大结果集下是主要热点）
  std::string out;
  out.reserve(256 + 128 * static_cast<size_t>(f.limit > 0 ? f.limit : 1));
  out += '[';

  if (args.empty()) {  // /studies
    auto studies = ctx_.dao->QueryStudies(f);
    bool first = true;
    for (const auto& s : studies) {
      if (!first) out += ',';
      first = false;
      out += "{\"0020000D\":{\"vr\":\"UI\",\"Value\":[\"";
      JsonEscapeInto(out, s.study_uid);
      out += "\"]},\"00100020\":{\"vr\":\"LO\",\"Value\":[\"";
      JsonEscapeInto(out, s.patient_id);
      out += "\"]},\"00080020\":{\"vr\":\"DA\",\"Value\":[\"";
      JsonEscapeInto(out, s.study_date);
      out += "\"]}}";
    }
  } else if (args.size() == 1) {  // /studies/{study}/series
    auto series = ctx_.dao->QuerySeries(args[0]);
    bool first = true;
    for (const auto& s : series) {
      if (!first) out += ',';
      first = false;
      out += "{\"0020000E\":{\"vr\":\"UI\",\"Value\":[\"";
      JsonEscapeInto(out, s.series_uid);
      out += "\"]},\"00080060\":{\"vr\":\"CS\",\"Value\":[\"";
      JsonEscapeInto(out, s.modality);
      out += "\"]}}";
    }
  } else {  // /studies/{study}/series/{series}/instances
    auto inst = ctx_.dao->QueryInstances(args[1]);
    bool first = true;
    for (const auto& i : inst) {
      if (!first) out += ',';
      first = false;
      out += "{\"00080018\":{\"vr\":\"UI\",\"Value\":[\"";
      JsonEscapeInto(out, i.sop_uid);
      out += "\"]}}";
    }
  }
  out += ']';
  if (cache_ok) {
    ctx_.cache->Set(cache_key, out, 60);
  }
  HttpResponse resp = HttpResponse::Json(out);
  if (cache_ok) resp.headers["x-cache"] = "MISS";
  return resp;
}

HttpResponse Router::HandleWado(const HttpRequest& req, const Args& args) const {
  if (!ctx_.dao) return HttpResponse::Error(500, "dao not configured");
  if (args.size() < 2) return HttpResponse::Error(400, "missing series uid");

  auto instances = ctx_.dao->QueryInstances(args[1]);
  if (instances.empty()) return HttpResponse::Error(404, "series not found");

  // 单实例下载
  if (args.size() >= 3) {
    for (const auto& i : instances) {
      if (i.sop_uid == args[2]) {
        // 依 Accept 决定返回形式：PS3.18 默认 multipart/related（与 Orthanc 等实现一致），
        // 仅当客户端明确不接受 multipart 时才返回单个 application/dicom。
        const std::string* acc = req.FindHeader("accept");
        const bool single = acc && acc->find("multipart/related") == std::string::npos;

        if (single) {
          HttpResponse resp = HttpResponse::File(i.file_path);
          if (req.range) {
            struct stat st{};
            uint64_t total = 0;
            if (::stat(i.file_path.c_str(), &st) == 0) total = static_cast<uint64_t>(st.st_size);
            uint64_t start = req.range->start;
            uint64_t end = req.range->end;
            if (end == UINT64_MAX || end >= total) end = total > 0 ? total - 1 : 0;
            if (total > 0 && start <= end && start < total) {
              resp.status = 206;
              resp.file_offset = start;
              resp.file_length = end - start + 1;
              resp.headers["Content-Range"] = "bytes " + std::to_string(start) + "-" +
                                              std::to_string(end) + "/" + std::to_string(total);
            }
          }
          resp.headers["Accept-Ranges"] = "bytes";
          return resp;
        }

        HttpResponse::Part p;
        p.file_path = i.file_path;
        p.content_type = "application/dicom";
        std::vector<HttpResponse::Part> parts;
        parts.push_back(std::move(p));
        const std::string boundary =
            "dicomweb-" + std::to_string(std::hash<std::string>{}(i.sop_uid));
        return HttpResponse::Multipart(boundary, std::move(parts));
      }
    }
    return HttpResponse::Error(404, "instance not found");
  }

  // 整个 series：multipart/related 聚合多个实例（内容走 sendfile 零拷贝，不整体驻留内存）
  std::vector<HttpResponse::Part> parts;
  parts.reserve(instances.size());
  for (const auto& i : instances) {
    HttpResponse::Part p;
    p.file_path = i.file_path;
    p.content_type = "application/dicom";
    parts.push_back(std::move(p));
  }
  const std::string boundary =
      "dicomweb-" + std::to_string(std::hash<std::string>{}(args[0] + "/" + args[1]));
  return HttpResponse::Multipart(boundary, std::move(parts));
}

HttpResponse Router::HandleStow(const HttpRequest& req, const Args& args) const {
  if (!ctx_.dao || !ctx_.store) return HttpResponse::Error(500, "store not configured");
  (void)args;

  // 收集待入库的实例字节流：优先 multipart 分段（解析器已切好），否则视为单个 DICOM
  std::vector<const std::string*> blobs;
  if (!req.multipart_parts.empty()) {
    for (const auto& p : req.multipart_parts) blobs.push_back(&p.body);
  } else if (!req.body.empty()) {
    blobs.push_back(&req.body);
  } else {
    return HttpResponse::Error(400, "empty payload");
  }

  std::ostringstream refs, fails;
  int ok = 0, fail = 0;
  std::set<std::string> touched_studies, touched_series;

  for (const std::string* blob : blobs) {
    // 1) 先落临时文件（未解析出 UID 前不污染正式命名空间）
    std::string err, tmp_path;
    if (!ctx_.store->WriteTemp(*blob, &tmp_path, &err)) {
      if (fail++) fails << ",";
      fails << "{\"FailureReason\":\"store:" << JsonEscape(err) << "\"}";
      continue;
    }

    // 2) 解析 DICOM 标签；未装 DCMTK 时用内容哈希派生占位 UID
    std::string derr;
    auto tags = DicomFile::ReadTags(tmp_path, &derr);
    std::string sop_uid, study_uid, series_uid, patient_id, study_date, modality, patient_name;
    if (tags) {
      sop_uid = tags->sop_instance_uid;
      study_uid = tags->study_instance_uid;
      series_uid = tags->series_instance_uid;
      patient_id = tags->patient_id;
      patient_name = tags->patient_name;
      study_date = tags->study_date;
      modality = tags->modality;
    }
    if (sop_uid.empty())
      sop_uid = "2.25." + std::to_string(std::hash<std::string>{}(*blob));
    if (study_uid.empty()) study_uid = "1.2.826.0.1.3680043.10.1000." + sop_uid;
    if (series_uid.empty()) series_uid = study_uid + ".1";

    // 3) 原子改名为正式对象
    if (!ctx_.store->Commit(tmp_path, sop_uid, &err)) {
      if (fail++) fails << ",";
      fails << "{\"FailureReason\":\"commit:" << JsonEscape(err) << "\"}";
      continue;
    }

    // 4) 幂等写元数据（三个层级）
    StudyRecord s;
    s.study_uid = study_uid;
    s.patient_id = patient_id;
    s.patient_name = patient_name;
    s.study_date = study_date;
    s.modality = modality;
    ctx_.dao->UpsertStudy(s);

    SeriesRecord se;
    se.series_uid = series_uid;
    se.study_uid = study_uid;
    se.modality = modality;
    ctx_.dao->UpsertSeries(se);

    InstanceRecord in;
    in.sop_uid = sop_uid;
    in.series_uid = series_uid;
    in.file_path = ctx_.store->PathFor(sop_uid);
    in.size_bytes = static_cast<int64_t>(blob->size());
    ctx_.dao->UpsertInstance(in);
    touched_studies.insert(study_uid);
    touched_series.insert(series_uid);

    if (ok++) refs << ",";
    refs << "{\"ReferencedSOPClassUID\":\"1.2.840.10008.5.1.4.1.1.7\","
            "\"ReferencedSOPInstanceUID\":\""
         << JsonEscape(sop_uid) << "\"}";
  }

  // 有新实例入库：清掉相关 QIDO 缓存（studies 列表 + 该 study 的 series + 该 series 的 instances）
  if (ctx_.cache && ok > 0) {
    for (const auto& sid : touched_studies) ctx_.cache->InvalidateStudy(sid);
    for (const auto& uid : touched_series) ctx_.cache->InvalidateSeries(uid);
  }

  std::ostringstream os;
  os << "{\"ReferencedSOPSequence\":[" << refs.str() << "],\"FailedSOPSequence\":[" << fails.str()
     << "],\"ok\":" << ok << ",\"failed\":" << fail << "}";
  return HttpResponse::Json(os.str(), ok > 0 ? 200 : 400);
}

}  // namespace dws
