#pragma once

#include <atomic>
#include <cstdint>

namespace cambang::capture_latency_trace_diagnostics {

// BEGIN TEMPORARY CAPTURE LATENCY DIAGNOSTICS
// Shared, process-local capture window state for temporary log correlation only.
// Remove this header and all call sites after capture latency logs are collected.
inline std::atomic<uint32_t> g_active_capture_count{0};

inline void note_capture_admitted(uint32_t device_count) noexcept {
  g_active_capture_count.fetch_add(device_count, std::memory_order_relaxed);
}

inline void note_capture_finished() noexcept {
  uint32_t current = g_active_capture_count.load(std::memory_order_relaxed);
  while (current != 0 &&
         !g_active_capture_count.compare_exchange_weak(
             current, current - 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
  }
}

inline uint32_t active_capture_count() noexcept {
  return g_active_capture_count.load(std::memory_order_relaxed);
}

inline uint32_t capture_inflight() noexcept {
  return active_capture_count() == 0 ? 0u : 1u;
}
// END TEMPORARY CAPTURE LATENCY DIAGNOSTICS

} // namespace cambang::capture_latency_trace_diagnostics
