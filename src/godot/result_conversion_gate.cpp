#include "godot/result_conversion_gate.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace cambang {

namespace {

// Process lifetime, never destroyed: a worker may still reach this state while
// static destructors run at exit.
struct GateState final {
  std::mutex mutex;
  std::condition_variable idle;
  bool open = false;
  uint64_t in_flight = 0;

  std::mutex observations_mutex;
  std::vector<CaptureObservation> observations;
};

GateState& gate() {
  static GateState* state = new GateState();
  return *state;
}

constexpr size_t kMaxOffMainCaptureObservations = 256;
// A conversion is pure CPU work of tens of milliseconds at most; the bound only
// keeps a wedged worker from hanging engine shutdown, and is reported if hit.
constexpr auto kTeardownWait = std::chrono::seconds(5);

} // namespace

bool result_conversion_on_main_thread() {
  godot::OS* os = godot::OS::get_singleton();
  return !os || os->get_thread_caller_id() == os->get_main_thread_id();
}

ResultConversionScope::ResultConversionScope() {
  GateState& g = gate();
  std::lock_guard<std::mutex> lock(g.mutex);
  if (g.open) {
    ++g.in_flight;
    admitted_ = true;
  }
}

ResultConversionScope::~ResultConversionScope() {
  if (!admitted_) {
    return;
  }
  GateState& g = gate();
  std::lock_guard<std::mutex> lock(g.mutex);
  if (--g.in_flight == 0) {
    g.idle.notify_all();
  }
}

void result_conversion_gate_open() {
  // godot-cpp resolves a singleton lazily into an unsynchronized static on its
  // first call; make that first call here, on the main thread, so a worker's
  // main-thread check only ever reads it.
  (void)godot::OS::get_singleton();
  GateState& g = gate();
  std::lock_guard<std::mutex> lock(g.mutex);
  g.open = true;
}

void result_conversion_gate_close_and_wait() {
  GateState& g = gate();
  std::unique_lock<std::mutex> lock(g.mutex);
  g.open = false;
  if (!g.idle.wait_for(lock, kTeardownWait, [&g] { return g.in_flight == 0; })) {
    const uint64_t remaining = g.in_flight;
    lock.unlock();
    godot::UtilityFunctions::push_error(
        "[CamBANG] extension teardown: ", static_cast<int64_t>(remaining),
        " result conversion(s) still running after 5 s; tearing down regardless.");
  }
}

void queue_off_main_capture_observation(const SharedCaptureResultData& data, uint32_t image_member_index) {
  if (!data) {
    return;
  }
  GateState& g = gate();
  std::lock_guard<std::mutex> lock(g.observations_mutex);
  if (g.observations.size() < kMaxOffMainCaptureObservations) {
    g.observations.emplace_back(data, image_member_index);
  }
}

std::vector<CaptureObservation> take_off_main_capture_observations() {
  std::vector<CaptureObservation> out;
  GateState& g = gate();
  std::lock_guard<std::mutex> lock(g.observations_mutex);
  out.swap(g.observations);
  return out;
}

void discard_off_main_capture_observations() {
  // Released on return, on the main thread, outside the queue lock.
  std::vector<CaptureObservation> dropped = take_off_main_capture_observations();
}

} // namespace cambang
