#pragma once
// 极简线程安全日志（header-only）。生产可替换为 spdlog。
#include <cstdio>
#include <mutex>
#include <string>
#include <ctime>

namespace dws {

enum class LogLevel { kDebug = 0, kInfo, kWarn, kError };

class Logger {
 public:
  static Logger& Instance() {
    static Logger inst;
    return inst;
  }

  void set_level(LogLevel lv) { level_ = lv; }

  template <typename... Args>
  void Log(LogLevel lv, const char* file, int line, const char* fmt, Args... args) {
    if (lv < level_) return;
    char body[1024];
    if constexpr (sizeof...(args) == 0) {
      std::snprintf(body, sizeof(body), "%s", fmt);  // 无参时按字面量输出，避免 -Wformat-security
    } else {
      std::snprintf(body, sizeof(body), fmt, args...);
    }
    std::lock_guard<std::mutex> lk(mu_);
    std::printf("[%s][%s:%d] %s\n", Now(), file, line, body);
    std::fflush(stdout);
  }

 private:
  Logger() = default;

  // 可移植的本地时间（Windows: localtime_s / POSIX: localtime_r）
  static void LocalTime(std::time_t t, std::tm* out) {
#if defined(_WIN32)
    localtime_s(out, &t);
#else
    localtime_r(&t, out);
#endif
  }

  static const char* Now() {
    static thread_local char buf[32];
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    LocalTime(t, &tm);
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
  }
  LogLevel level_ = LogLevel::kInfo;
  std::mutex mu_;
};

}  // namespace dws

#define LOG_DEBUG(...) ::dws::Logger::Instance().Log(::dws::LogLevel::kDebug, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  ::dws::Logger::Instance().Log(::dws::LogLevel::kInfo,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  ::dws::Logger::Instance().Log(::dws::LogLevel::kWarn,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) ::dws::Logger::Instance().Log(::dws::LogLevel::kError, __FILE__, __LINE__, __VA_ARGS__)
