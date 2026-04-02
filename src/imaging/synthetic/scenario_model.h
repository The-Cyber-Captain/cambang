#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "imaging/api/provider_contract_datatypes.h"
#include "imaging/synthetic/scenario.h"

namespace cambang {

// Canonical/authored synthetic scenario projection.
// This layer keeps authored symbolic identity and timeline intent distinct from
// provider-executable schedule ids.

struct SyntheticScenarioDeviceDeclaration {
  std::string key;
  std::uint32_t endpoint_index = 0;
};

struct SyntheticScenarioStreamDeclaration {
  std::string key;
  std::string device_key;
  StreamIntent intent = StreamIntent::PREVIEW;
  CaptureProfile baseline_capture_profile{};
  bool realize_baseline_lifecycle = true;
  bool has_baseline_picture = false;
  PictureConfig baseline_picture{};
};

struct SyntheticScenarioTimelineAction {
  std::uint64_t at_ns = 0;
  SyntheticEventType type = SyntheticEventType::EmitFrame;

  // Symbolic targets resolved during materialization.
  std::string device_key;
  std::string stream_key;

  bool has_picture = false;
  PictureConfig picture{};
};

struct SyntheticCanonicalScenario {
  std::vector<SyntheticScenarioDeviceDeclaration> devices;
  std::vector<SyntheticScenarioStreamDeclaration> streams;
  std::vector<SyntheticScenarioTimelineAction> timeline;
};

struct SyntheticScenarioMaterializationOptions {
  std::uint64_t device_instance_id_base = 10'000;
  std::uint64_t root_id_base = 20'000;
  std::uint64_t stream_id_base = 30'000;
};

struct SyntheticMaterializedDeviceBinding {
  std::string key;
  std::uint32_t endpoint_index = 0;
  std::uint64_t device_instance_id = 0;
  std::uint64_t root_id = 0;
};

struct SyntheticMaterializedStreamBinding {
  std::string key;
  std::string device_key;
  StreamIntent intent = StreamIntent::PREVIEW;
  CaptureProfile baseline_capture_profile{};
  bool realize_baseline_lifecycle = true;
  bool has_baseline_picture = false;
  PictureConfig baseline_picture{};
  std::uint64_t device_instance_id = 0;
  std::uint64_t stream_id = 0;
};

struct SyntheticScenarioMaterializationResult {
  std::vector<SyntheticMaterializedDeviceBinding> devices;
  std::vector<SyntheticMaterializedStreamBinding> streams;
  SyntheticTimelineScenario executable_schedule{};
};

bool materialize_synthetic_canonical_scenario(
    const SyntheticCanonicalScenario& canonical,
    const SyntheticScenarioMaterializationOptions& options,
    SyntheticScenarioMaterializationResult& out,
    std::string* error = nullptr);

} // namespace cambang
