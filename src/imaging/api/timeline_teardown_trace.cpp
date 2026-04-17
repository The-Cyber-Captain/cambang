#include "imaging/api/timeline_teardown_trace.h"

#include <atomic>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace cambang {

namespace {

std::mutex& trace_mu() {
  static std::mutex mu;
  return mu;
}

std::deque<std::string>& trace_q() {
  static std::deque<std::string> q;
  return q;
}

constexpr size_t kTraceQueueCap = 256;

std::atomic<bool>& trace_enabled() {
  static std::atomic<bool> enabled{false};
  return enabled;
}

bool parse_env_enabled() {
  const char* value = std::getenv("CAMBANG_TIMELINE_TEARDOWN_TRACE");
  if (!value) {
    return false;
  }
  return value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
}

void init_from_env_once() {
  static const bool initialized = []() {
    trace_enabled().store(parse_env_enabled(), std::memory_order_relaxed);
    return true;
  }();
  (void)initialized;
}

std::string normalize_line(std::string line) {
  constexpr const char* kPrefix = "[timeline_teardown]";
  if (line.rfind(kPrefix, 0) == 0) {
    line.erase(0, std::char_traits<char>::length(kPrefix));
    while (!line.empty() && line.front() == ' ') {
      line.erase(line.begin());
    }
  }

  auto with_tag = [&](const char* tag, const char* token) -> std::string {
    const size_t token_len = std::char_traits<char>::length(token);
    if (line.rfind(token, 0) == 0) {
      std::string body = line.substr(token_len);
      if (!body.empty() && body.front() == ' ') {
        body.erase(body.begin());
      }
      return std::string("[timeline_teardown][") + tag + "] " + body;
    }
    return {};
  };

  if (auto tagged = with_tag("FAIL", "fail"); !tagged.empty()) {
    return tagged;
  }
  if (auto tagged = with_tag("PENDING", "pending"); !tagged.empty()) {
    return tagged;
  }
  if (auto tagged = with_tag("COMPLETE", "complete"); !tagged.empty()) {
    return tagged;
  }

  return std::string("[timeline_teardown] ") + line;
}

} // namespace

void timeline_teardown_trace_set_enabled(bool enabled) {
  init_from_env_once();
  trace_enabled().store(enabled, std::memory_order_relaxed);
}

bool timeline_teardown_trace_enabled() {
  init_from_env_once();
  return trace_enabled().load(std::memory_order_relaxed);
}

void timeline_teardown_trace_emit(const char* fmt, ...) {
  init_from_env_once();
  if (!timeline_teardown_trace_enabled()) {
    return;
  }

  if (!fmt) {
    return;
  }

  char msg[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  std::string line = normalize_line(msg);

  std::lock_guard<std::mutex> lock(trace_mu());
  auto& q = trace_q();
  q.push_back(line);
  if (q.size() > kTraceQueueCap) {
    q.pop_front();
  }
}

bool timeline_teardown_trace_try_pop(std::string& out) {
  std::lock_guard<std::mutex> lock(trace_mu());
  auto& q = trace_q();
  if (q.empty()) {
    return false;
  }
  out = std::move(q.front());
  q.pop_front();
  return true;
}

} // namespace cambang
