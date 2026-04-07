#pragma once

#include <string>

namespace cambang {

void timeline_teardown_trace_set_enabled(bool enabled);
bool timeline_teardown_trace_enabled();

void timeline_teardown_trace_emit(const char* fmt, ...);
bool timeline_teardown_trace_try_pop(std::string& out);

} // namespace cambang
