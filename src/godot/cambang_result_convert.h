#pragma once

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include "core/core_result_store.h"

namespace cambang {

godot::Dictionary to_dict(const ResultImagePropertiesFacts& v);
godot::Dictionary to_dict(const ResultCaptureAttributesFacts& v);
godot::Dictionary to_dict(const ResultLocationAttributesFacts& v);
godot::Dictionary to_dict(const ResultOpticalCalibrationFacts& v);

godot::Dictionary to_dict(const ResultImagePropertiesProvenance& v);
godot::Dictionary to_dict(const ResultCaptureAttributesProvenance& v);
godot::Dictionary to_dict(const ResultLocationAttributesProvenance& v);
godot::Dictionary to_dict(const ResultOpticalCalibrationProvenance& v);

godot::Ref<godot::Image> payload_to_image(const CoreResultPayloadCpuPacked& payload);

} // namespace cambang
