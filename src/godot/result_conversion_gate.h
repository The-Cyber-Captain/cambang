#pragma once

// Thread admission for the public result conversions (CaptureResult.to_image(),
// CaptureResult.to_image_member(), StreamResult.to_image()), which the result
// contract allows on any thread.
//
// A conversion off the main thread reads the result data held by the caller's
// own reference, plus mutex-guarded bookkeeping. The hazard is extension
// teardown: CamBANGServer and the GPU bridges are destroyed while a conversion
// that started earlier may still be running. Godot 4.5.2 idles its worker pool
// and shuts GDScript down before it tears CamBANG down, so no new GDScript call
// can arrive then, but a call already in flight can. Teardown therefore closes
// this gate first and waits for every admitted conversion to leave before
// anything is destroyed; a conversion arriving after that returns null. The
// gate's own state is trivially destructible, so that refusal holds even for a
// call arriving after static destructors have run. Everything else here is an
// ordinary destructible object: a caller finishes or waits for its threads
// before quit.
//
// The capture-result observation a conversion feeds to retained-access
// calibration reads main-thread-owned server state, so off the main thread it
// is queued here and reported by the server's main-thread tick: a worker never
// reaches the server itself.

#include <cstdint>
#include <utility>
#include <vector>

#include "core/core_result_store.h"

namespace cambang {

bool result_conversion_on_main_thread();

// RAII admission. A conversion that is not admitted (extension teardown has
// begun) must return null without touching anything else.
class ResultConversionScope final {
public:
  ResultConversionScope();
  ~ResultConversionScope();
  ResultConversionScope(const ResultConversionScope&) = delete;
  ResultConversionScope& operator=(const ResultConversionScope&) = delete;
  bool admitted() const { return admitted_; }

private:
  bool admitted_ = false;
};

// Extension initialization: admit conversions.
void result_conversion_gate_open();
// Extension teardown, before CamBANGServer or any bridge is destroyed: refuse
// new conversions, then wait for the admitted ones to finish.
void result_conversion_gate_close_and_wait();

using CaptureObservation = std::pair<SharedCaptureResultData, uint32_t>;

// Any thread. Bounded; an observation beyond the bound is calibration input
// lost, never a result.
void queue_off_main_capture_observation(const SharedCaptureResultData& data, uint32_t image_member_index);
// Main thread.
std::vector<CaptureObservation> take_off_main_capture_observations();
// Main thread, at stop(): an observation still queued then belongs to the
// ending session, where a main-thread call would already have reported it; it
// is not reported against the next one. (One queued after stop() is reported
// exactly as a main-thread call after stop() would be.)
void discard_off_main_capture_observations();

} // namespace cambang
