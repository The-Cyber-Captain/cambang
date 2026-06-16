#pragma once

#include "core/core_result_store.h"

namespace cambang::retained_result_access_calibration {

void calibrate_stream_result(const SharedStreamResultData& data);
void calibrate_capture_result(const SharedCaptureResultData& data);
void clear() noexcept;

} // namespace cambang::retained_result_access_calibration
