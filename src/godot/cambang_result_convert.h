#pragma once

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include "core/core_result_store.h"
#include "godot/cambang_result_convert_timing.h"

namespace cambang {

godot::Dictionary to_dict(const ResultImagePropertiesFacts& v);
godot::Dictionary to_dict(const ResultCaptureAttributesFacts& v);

godot::Dictionary to_dict(const ResultImagePropertiesProvenance& v);
godot::Dictionary to_dict(const ResultCaptureAttributesProvenance& v);

godot::Ref<godot::Image> payload_to_image(const CoreResultPayloadCpuPacked& payload);

} // namespace cambang
