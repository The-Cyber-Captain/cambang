#pragma once

#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>

namespace cli {
namespace detail {

inline std::mutex& line_mutex() {
  static std::mutex m;
  return m;
}

using line_sink_fn = void (*)(FILE* stream, const std::string& text, void* user);

struct line_sink {
  line_sink_fn fn = nullptr;
  void* user = nullptr;
};

inline line_sink& active_sink() {
  static line_sink sink{};
  return sink;
}

inline void write_line_unlocked(FILE* stream, const std::string& text) {
  std::fwrite(text.data(), 1, text.size(), stream);
  std::fputc('\n', stream);
  std::fflush(stream);
}

inline void write_line(FILE* stream, const std::string& text) {
  std::lock_guard<std::mutex> lock(line_mutex());
  const line_sink sink = active_sink();
  if (sink.fn) {
    sink.fn(stream, text, sink.user);
    return;
  }
  write_line_unlocked(stream, text);
}

} // namespace detail

class scoped_line_sink final {
public:
  scoped_line_sink(detail::line_sink_fn fn, void* user) {
    std::lock_guard<std::mutex> lock(detail::line_mutex());
    previous_ = detail::active_sink();
    detail::active_sink() = detail::line_sink{fn, user};
  }

  ~scoped_line_sink() {
    std::lock_guard<std::mutex> lock(detail::line_mutex());
    detail::active_sink() = previous_;
  }

  scoped_line_sink(const scoped_line_sink&) = delete;
  scoped_line_sink& operator=(const scoped_line_sink&) = delete;

private:
  detail::line_sink previous_{};
};

template<typename... Args>
inline void line(Args&&... args) {
  std::ostringstream ss;
  (ss << ... << args);
  detail::write_line(stdout, ss.str());
}

inline void blank() {
  detail::write_line(stdout, std::string{});
}

template<typename... Args>
inline void warn(Args&&... args) {
  std::ostringstream ss;
  ss << "[WARN] ";
  (ss << ... << args);
  detail::write_line(stdout, ss.str());
}

template<typename... Args>
inline void error(Args&&... args) {
  std::ostringstream ss;
  ss << "[ERROR] ";
  (ss << ... << args);
  detail::write_line(stderr, ss.str());
}

} // namespace cli
