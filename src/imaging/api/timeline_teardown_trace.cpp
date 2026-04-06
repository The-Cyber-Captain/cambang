#include "imaging/api/timeline_teardown_trace.h"

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

} // namespace

void timeline_teardown_trace_emit(const char* fmt, ...) {
  if (!fmt) {
    return;
  }

  char msg[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  std::string line = msg;
  if (line.rfind("[timeline_teardown]", 0) != 0) {
    line = std::string("[timeline_teardown] ") + line;
  }

  std::fprintf(stdout, "%s\n", line.c_str());
  std::fflush(stdout);

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

