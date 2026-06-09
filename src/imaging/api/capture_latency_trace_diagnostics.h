#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if __has_include(<godot_cpp/variant/utility_functions.hpp>)
#include <godot_cpp/variant/utility_functions.hpp>
#define CAMBANG_CAPTURE_LATENCY_TRACE_HAS_GODOT_PRINT 1
#else
#define CAMBANG_CAPTURE_LATENCY_TRACE_HAS_GODOT_PRINT 0
#endif

namespace cambang::capture_latency_trace_diagnostics {

// BEGIN TEMPORARY CAPTURE LATENCY DIAGNOSTICS
// Shared, process-local capture latency trace helpers for temporary log
// correlation only. Remove this header and all call sites after capture latency
// logs are collected.
inline std::atomic<uint32_t> g_active_capture_count{0};
inline std::atomic<bool> g_seen_core_device_future{false};
inline std::atomic<bool> g_seen_broker_trigger_capture{false};
inline std::atomic<bool> g_seen_broker_trigger_submission{false};
inline std::atomic<bool> g_seen_strand_deliver_capture_completed{false};
inline std::atomic<bool> g_seen_core_ingress_dispatch{false};
inline std::atomic<bool> g_seen_result_store_retain_frame{false};

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

inline bool starts_with(const char* text, const char* prefix) noexcept {
  return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

inline void note_trace_label_seen(const char* message) noexcept {
  if (starts_with(message, "core_device_future")) {
    g_seen_core_device_future.store(true, std::memory_order_relaxed);
  } else if (starts_with(message, "broker_trigger_capture")) {
    g_seen_broker_trigger_capture.store(true, std::memory_order_relaxed);
  } else if (starts_with(message, "broker_trigger_submission")) {
    g_seen_broker_trigger_submission.store(true, std::memory_order_relaxed);
  } else if (starts_with(message, "strand_deliver_capture_completed")) {
    g_seen_strand_deliver_capture_completed.store(true, std::memory_order_relaxed);
  } else if (starts_with(message, "core_ingress_dispatch")) {
    g_seen_core_ingress_dispatch.store(true, std::memory_order_relaxed);
  } else if (starts_with(message, "result_store_retain_frame")) {
    g_seen_result_store_retain_frame.store(true, std::memory_order_relaxed);
  }
}

inline void print_line(const char* message) {
  note_trace_label_seen(message);
#if CAMBANG_CAPTURE_LATENCY_TRACE_HAS_GODOT_PRINT
  std::string line = "[CamBANG][CaptureLatencyTrace] ";
  line += message;
  godot::UtilityFunctions::print(line.c_str());
#else
  std::fprintf(stdout, "[CamBANG][CaptureLatencyTrace] %s\n", message);
#endif
}

inline void reset_trace_group_seen() noexcept {
  g_seen_core_device_future.store(false, std::memory_order_relaxed);
  g_seen_broker_trigger_capture.store(false, std::memory_order_relaxed);
  g_seen_broker_trigger_submission.store(false, std::memory_order_relaxed);
  g_seen_strand_deliver_capture_completed.store(false, std::memory_order_relaxed);
  g_seen_core_ingress_dispatch.store(false, std::memory_order_relaxed);
  g_seen_result_store_retain_frame.store(false, std::memory_order_relaxed);
}

inline void print_trace_group_seen_summary() {
  char buffer[384];
  std::snprintf(buffer,
                sizeof(buffer),
                "trace_group_seen core_device_future=%u broker_trigger_capture=%u broker_trigger_submission=%u strand_deliver_capture_completed=%u core_ingress_dispatch=%u result_store_retain_frame=%u",
                g_seen_core_device_future.load(std::memory_order_relaxed) ? 1u : 0u,
                g_seen_broker_trigger_capture.load(std::memory_order_relaxed) ? 1u : 0u,
                g_seen_broker_trigger_submission.load(std::memory_order_relaxed) ? 1u : 0u,
                g_seen_strand_deliver_capture_completed.load(std::memory_order_relaxed) ? 1u : 0u,
                g_seen_core_ingress_dispatch.load(std::memory_order_relaxed) ? 1u : 0u,
                g_seen_result_store_retain_frame.load(std::memory_order_relaxed) ? 1u : 0u);
  print_line(buffer);
}
// END TEMPORARY CAPTURE LATENCY DIAGNOSTICS

} // namespace cambang::capture_latency_trace_diagnostics
