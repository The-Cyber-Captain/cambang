#pragma once

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <optional>

#include "core/core_result_store.h"

namespace cambang {

godot::Dictionary to_dict(const SourcedFact<ImageAcquisitionTiming>& v);
void add_acquisition_timing_camera_fact(
    godot::Dictionary& out,
    const std::optional<SourcedFact<ImageAcquisitionTiming>>& v);

}  // namespace cambang
