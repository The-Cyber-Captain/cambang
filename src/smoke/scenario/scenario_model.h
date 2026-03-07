#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cambang {

using ScenarioTime = std::uint64_t;

enum class ScenarioEventType : std::uint8_t {
  OpenDevice = 0,
  CloseDevice,
  CreateStream,
  StartStream,
  StopStream,
  DestroyStream,
  EmitFrame,
  InjectStopError,
};

enum class ScenarioProviderMask : std::uint8_t {
  None = 0,
  Synthetic = 1 << 0,
  Stub = 1 << 1,
  Any = Synthetic | Stub,
};

constexpr inline ScenarioProviderMask operator|(ScenarioProviderMask a, ScenarioProviderMask b) {
  return static_cast<ScenarioProviderMask>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

constexpr inline bool provider_mask_contains(ScenarioProviderMask mask, ScenarioProviderMask value) {
  return (static_cast<std::uint8_t>(mask) & static_cast<std::uint8_t>(value)) != 0;
}

struct ScenarioEvent {
  ScenarioTime at = 0;
  ScenarioEventType type = ScenarioEventType::OpenDevice;
  std::string id;
};

struct ScenarioExpectation {
  ScenarioTime at = 0;
  std::uint32_t device_count = 0;
  std::uint32_t stream_count = 0; // Flowing stream count in this tranche.
  bool require_topology_change = false;
};

struct Scenario {
  struct AtBuilder {
    Scenario* scenario = nullptr;
    ScenarioTime at = 0;

    AtBuilder& open_device(const std::string& id) {
      scenario->events.push_back({at, ScenarioEventType::OpenDevice, id});
      return *this;
    }
    AtBuilder& close_device(const std::string& id) {
      scenario->events.push_back({at, ScenarioEventType::CloseDevice, id});
      return *this;
    }
    AtBuilder& create_stream(const std::string& id) {
      scenario->events.push_back({at, ScenarioEventType::CreateStream, id});
      return *this;
    }
    AtBuilder& start_stream(const std::string& id) {
      scenario->events.push_back({at, ScenarioEventType::StartStream, id});
      return *this;
    }
    AtBuilder& stop_stream(const std::string& id) {
      scenario->events.push_back({at, ScenarioEventType::StopStream, id});
      return *this;
    }
    AtBuilder& destroy_stream(const std::string& id) {
      scenario->events.push_back({at, ScenarioEventType::DestroyStream, id});
      return *this;
    }
    AtBuilder& emit_frame(const std::string& id) {
      scenario->events.push_back({at, ScenarioEventType::EmitFrame, id});
      return *this;
    }
    AtBuilder& inject_stop_error(const std::string& id) {
      scenario->events.push_back({at, ScenarioEventType::InjectStopError, id});
      return *this;
    }
    AtBuilder& expect(std::uint32_t device_count,
                      std::uint32_t stream_count,
                      bool require_topology_change = false) {
      scenario->expectations.push_back({at, device_count, stream_count, require_topology_change});
      return *this;
    }
  };

  std::string name;
  ScenarioProviderMask provider_mask = ScenarioProviderMask::Any;
  std::vector<ScenarioEvent> events;
  std::vector<ScenarioExpectation> expectations;

  explicit Scenario(std::string name_in, ScenarioProviderMask providers = ScenarioProviderMask::Any)
      : name(std::move(name_in)), provider_mask(providers) {}

  AtBuilder at(ScenarioTime time) { return AtBuilder{this, time}; }
};

} // namespace cambang
