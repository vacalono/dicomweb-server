#include "store/IMetadataDao.h"

#include "store/MetadataDao.h"
#include "store/MysqlMetadataDao.h"
#include "util/Logger.h"

namespace dws {

std::unique_ptr<IMetadataDao> CreateMetadataDao(const std::string& backend, const std::string& dsn) {
  if (backend.empty() || backend == "sqlite") {
    return std::make_unique<MetadataDao>(dsn);
  }
  if (backend == "mysql") {
    return std::make_unique<MysqlMetadataDao>(MysqlMetadataDao::ParseDsn(dsn));
  }
  LOG_ERROR("unknown metadata backend '%s' (expected sqlite|mysql)", backend.c_str());
  return nullptr;
}

}  // namespace dws
