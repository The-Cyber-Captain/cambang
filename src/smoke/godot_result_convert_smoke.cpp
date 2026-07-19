#include <cstdint>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "godot/cambang_result_convert_timing.h"
#include "smoke/godot_variant_runtime_minimal.h"

namespace {

std::optional<cambang::SourcedFact<cambang::ImageAcquisitionTiming>> make_timing_fact(
    int64_t acquisition_mark) {
  const auto tick_period = cambang::TickPeriod::create(10, 4);
  if (!tick_period) {
    throw std::runtime_error("failed to construct canonical timing period");
  }
  const auto timing = cambang::ImageAcquisitionTiming::create(
      acquisition_mark,
      *tick_period,
      cambang::ImageAcquisitionClockDomain::DOMAIN_OPAQUE,
      cambang::ImageAcquisitionReferenceEvent::EXPOSURE_MIDPOINT,
      cambang::ImageAcquisitionComparability::SAME_IMAGE_ONLY);
  if (!timing) {
    throw std::runtime_error("failed to construct image acquisition timing");
  }
  return cambang::SourcedFact<cambang::ImageAcquisitionTiming>{
      *timing,
      cambang::FactOrigin::NATIVE_REPORTED};
}

void verify_absence_case() {
  godot::Dictionary out;
  cambang::add_acquisition_timing_camera_fact(out, std::nullopt);
  if (cambang::smoke::dictionary_has_key(out, "acquisition_timing")) {
    throw std::runtime_error("absence case fabricated acquisition_timing");
  }
}

void verify_timing_case(int64_t acquisition_mark) {
  godot::Dictionary out;
  cambang::add_acquisition_timing_camera_fact(out, make_timing_fact(acquisition_mark));
  const godot::Variant timing_value =
      cambang::smoke::dictionary_find_value(out, "acquisition_timing");
  if (cambang::smoke::variant_type(timing_value) != GDEXTENSION_VARIANT_TYPE_DICTIONARY) {
    throw std::runtime_error("missing acquisition_timing dictionary");
  }
  const godot::Dictionary timing_dict = cambang::smoke::variant_to_dictionary(timing_value);

  const int64_t actual_mark = cambang::smoke::variant_int(
      cambang::smoke::dictionary_find_value(timing_dict, "acquisition_mark"));
  if (actual_mark != acquisition_mark) {
    throw std::runtime_error("acquisition_mark value mismatch");
  }

  const godot::Variant numerator =
      cambang::smoke::dictionary_find_value(timing_dict, "tick_period_numerator_ns");
  if (cambang::smoke::variant_type(numerator) != GDEXTENSION_VARIANT_TYPE_INT ||
      cambang::smoke::variant_int(numerator) != 5) {
    throw std::runtime_error("tick_period_numerator_ns was not reduced to 5");
  }
  const godot::Variant denominator =
      cambang::smoke::dictionary_find_value(timing_dict, "tick_period_denominator");
  if (cambang::smoke::variant_type(denominator) != GDEXTENSION_VARIANT_TYPE_INT ||
      cambang::smoke::variant_int(denominator) != 2) {
    throw std::runtime_error("tick_period_denominator was not reduced to 2");
  }

  if (cambang::smoke::variant_string(
          cambang::smoke::dictionary_find_value(timing_dict, "origin")) !=
      "native_reported") {
    throw std::runtime_error("origin string mismatch");
  }
  if (cambang::smoke::variant_string(
          cambang::smoke::dictionary_find_value(timing_dict, "clock_domain")) !=
      "domain_opaque") {
    throw std::runtime_error("clock_domain string mismatch");
  }
  if (cambang::smoke::variant_string(
          cambang::smoke::dictionary_find_value(timing_dict, "reference_event")) !=
      "exposure_midpoint") {
    throw std::runtime_error("reference_event string mismatch");
  }
  if (cambang::smoke::variant_string(
          cambang::smoke::dictionary_find_value(timing_dict, "comparability")) !=
      "same_image_only") {
    throw std::runtime_error("comparability string mismatch");
  }
}

template <typename T, typename Fn>
void construct_in_nonzero_storage(Fn&& fn) {
  alignas(T) std::array<std::byte, sizeof(T)> storage;
  storage.fill(std::byte{0xA5});
  T* value = fn(storage.data());
  value->~T();
}

void verify_nonzero_storage_construction() {
  construct_in_nonzero_storage<godot::Variant>([](void* storage) {
    return new (storage) godot::Variant();
  });
  construct_in_nonzero_storage<godot::Variant>([](void* storage) {
    return new (storage) godot::Variant(int64_t{17});
  });
  construct_in_nonzero_storage<godot::String>([](void* storage) {
    return new (storage) godot::String("nonzero");
  });
  construct_in_nonzero_storage<godot::Dictionary>([](void* storage) {
    return new (storage) godot::Dictionary();
  });
  construct_in_nonzero_storage<godot::Variant>([](void* storage) {
    godot::Dictionary dict;
    dict[godot::String("key")] = godot::Variant(int64_t{1});
    return new (storage) godot::Variant(dict);
  });
}

}  // namespace

int main() {
  try {
    cambang::smoke::install_fake_godot_runtime();
    verify_nonzero_storage_construction();
    verify_absence_case();
    verify_timing_case(0);
    verify_timing_case(std::numeric_limits<int64_t>::max());
    std::cout << "PASS godot_result_convert_smoke\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << "FAIL godot_result_convert_smoke reason=" << ex.what() << "\n";
    return EXIT_FAILURE;
  }
}
