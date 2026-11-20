#include "godot/result_conversion_gate.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <type_traits>

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace cambang {

namespace {

// The gate itself is trivially destructible, so it stays readable for the
// whole life of the process: a call that arrives after teardown, even after
// static destructors have run, reads a closed gate and returns null.
//
// Admission increments first and then checks open; teardown clears open and
// then waits for in_flight to reach zero. Both are sequentially consistent, so
// either teardown sees the increment and waits for it, or the conversion sees
// the gate closed and backs out.
std::atomic<bool> g_open{false};
std::atomic<uint64_t> g_in_flight{0};
static_assert(std::is_trivially_destructible_v<std::atomic<bool>>);
static_assert(std::is_trivially_destructible_v<std::atomic<uint64_t>>);

// Ordinary statics. Only an admitted conversion or the main thread reaches
// them, and teardown waits for the admitted conversions and then empties the
// queue (CamBANGServer::stop()) before static destructors run.
std::mutex g_observations_mutex;
std::vector<CaptureObservation> g_observations;

constexpr size_t kMaxOffMainCaptureObservations = 256;
// A conversion is pure CPU work of tens of milliseconds at most; the bound only
// keeps a wedged worker from hanging engine shutdown, and is reported if hit.
constexpr auto kTeardownWait = std::chrono::seconds(5);
constexpr auto kTeardownPoll = std::chrono::milliseconds(1);

} // namespace

bool result_conversion_on_main_thread() {
  godot::OS* os = godot::OS::get_singleton();
  return !os || os->get_thread_caller_id() == os->get_main_thread_id();
}

ResultConversionScope::ResultConversionScope() {
  g_in_flight.fetch_add(1);
  if (g_open.load()) {
    admitted_ = true;
  } else {
    g_in_flight.fetch_sub(1);
  }
}

ResultConversionScope::~ResultConversionScope() {
  if (admitted_) {
    g_in_flight.fetch_sub(1);
  }
}

void result_conversion_gate_open() {
  // godot-cpp resolves a singleton lazily into an unsynchronized static on its
  // first call; make that first call here, on the main thread, so a worker's
  // main-thread check only ever reads it.
  (void)godot::OS::get_singleton();
  g_open.store(true);
}

void result_conversion_gate_close_and_wait() {
  g_open.store(false);
  const auto deadline = std::chrono::steady_clock::now() + kTeardownWait;
  while (g_in_flight.load() != 0) {
    if (std::chrono::steady_clock::now() >= deadline) {
      godot::UtilityFunctions::push_error(
          "[CamBANG] extension teardown: ", static_cast<int64_t>(g_in_flight.load()),
          " result conversion(s) still running after 5 s; tearing down regardless.");
      return;
    }
    std::this_thread::sleep_for(kTeardownPoll);
  }
}

void queue_off_main_capture_observation(const SharedCaptureResultData& data, uint32_t image_member_index) {
  if (!data) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_observations_mutex);
  if (g_observations.size() < kMaxOffMainCaptureObservations) {
    g_observations.emplace_back(data, image_member_index);
  }
}

std::vector<CaptureObservation> take_off_main_capture_observations() {
  std::vector<CaptureObservation> out;
  std::lock_guard<std::mutex> lock(g_observations_mutex);
  out.swap(g_observations);
  return out;
}

void discard_off_main_capture_observations() {
  // Released on return, on the main thread, outside the queue lock.
  std::vector<CaptureObservation> dropped = take_off_main_capture_observations();
}

} // namespace cambang
