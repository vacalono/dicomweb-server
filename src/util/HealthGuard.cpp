#include "util/HealthGuard.h"

#include "store/IMetadataDao.h"
#include "store/QidoCache.h"
#include "util/Logger.h"

#include <chrono>

namespace dws {

HealthGuard::HealthGuard(IMetadataDao* dao, QidoCache* cache, int interval_ms)
    : dao_(dao), cache_(cache), interval_ms_(interval_ms) {}

HealthGuard::~HealthGuard() { Stop(); }

void HealthGuard::Start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this]() { Loop(); });
}

void HealthGuard::Stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

void HealthGuard::Loop() {
  while (running_.load()) {
    if (dao_) {
      if (dao_->Ping()) {
        db_breaker_.OnSuccess();
      } else {
        db_breaker_.OnFailure();
      }
    }
    if (cache_) {
      if (cache_->Ping()) {
        cache_breaker_.OnSuccess();
      } else {
        cache_breaker_.OnFailure();
      }
    }
    // 分段睡眠，Stop() 最多等一个 interval
    for (int slept = 0; slept < interval_ms_ && running_.load(); slept += 50) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
}

}  // namespace dws
