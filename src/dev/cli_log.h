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

inline void write_line(FILE* stream, const std::string& text) {
  std::lock_guard<std::mutex> lock(line_mutex());
  std::fwrite(text.data(), 1, text.size(), stream);
  std::fputc('\n', stream);
  std::fflush(stream);
}

} // namespace detail

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
