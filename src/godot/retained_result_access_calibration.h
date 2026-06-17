#pragma once

#include "core/core_result_store.h"

namespace cambang {
class CoreRuntime;
}

namespace cambang::retained_result_access_calibration {

void calibrate_stream_result(const SharedStreamResultData& data,
                             CoreRuntime* runtime = nullptr);
void calibrate_capture_result(const SharedCaptureResultData& data,
                              CoreRuntime* runtime = nullptr);
void clear() noexcept;

} // namespace cambang::retained_result_access_calibration
