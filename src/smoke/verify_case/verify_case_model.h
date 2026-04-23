#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cambang {

using VerifyCaseTime = std::uint64_t;

enum class VerifyCaseEventType : std::uint8_t {
  OpenDevice = 0,
  CloseDevice,
  CreateStream,
  StartStream,
  StopStream,
  DestroyStream,
  EmitFrame,
  InjectStopError,
};

enum class VerifyCaseProviderMask : std::uint8_t {
  None = 0,
  Synthetic = 1 << 0,
  Stub = 1 << 1,
  Any = Synthetic | Stub,
};

constexpr inline VerifyCaseProviderMask operator|(VerifyCaseProviderMask a, VerifyCaseProviderMask b) {
  return static_cast<VerifyCaseProviderMask>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

constexpr inline bool provider_mask_contains(VerifyCaseProviderMask mask, VerifyCaseProviderMask value) {
  return (static_cast<std::uint8_t>(mask) & static_cast<std::uint8_t>(value)) != 0;
}

struct VerifyCaseEvent {
  VerifyCaseTime at = 0;
  VerifyCaseEventType type = VerifyCaseEventType::OpenDevice;
  std::string id;
};

struct VerifyCaseExpectation {
  VerifyCaseTime at = 0;
  std::uint32_t device_count = 0;
  std::uint32_t stream_count = 0; // Flowing stream count in this tranche.
  std::uint32_t acquisition_session_count = 0;
  bool expect_acquisition_session = false;
  bool require_topology_change = false;
};

struct VerifyCase {
  struct AtBuilder {
    VerifyCase* verify_case = nullptr;
    VerifyCaseTime at = 0;

    AtBuilder& open_device(const std::string& id) {
      verify_case->events.push_back({at, VerifyCaseEventType::OpenDevice, id});
      return *this;
    }
    AtBuilder& close_device(const std::string& id) {
      verify_case->events.push_back({at, VerifyCaseEventType::CloseDevice, id});
      return *this;
    }
    AtBuilder& create_stream(const std::string& id) {
      verify_case->events.push_back({at, VerifyCaseEventType::CreateStream, id});
      return *this;
    }
    AtBuilder& start_stream(const std::string& id) {
      verify_case->events.push_back({at, VerifyCaseEventType::StartStream, id});
      return *this;
    }
    AtBuilder& stop_stream(const std::string& id) {
      verify_case->events.push_back({at, VerifyCaseEventType::StopStream, id});
      return *this;
    }
    AtBuilder& destroy_stream(const std::string& id) {
      verify_case->events.push_back({at, VerifyCaseEventType::DestroyStream, id});
      return *this;
    }
    AtBuilder& emit_frame(const std::string& id) {
      verify_case->events.push_back({at, VerifyCaseEventType::EmitFrame, id});
      return *this;
    }
    AtBuilder& inject_stop_error(const std::string& id) {
      verify_case->events.push_back({at, VerifyCaseEventType::InjectStopError, id});
      return *this;
    }
    AtBuilder& expect(std::uint32_t device_count,
                      std::uint32_t stream_count,
                      bool require_topology_change = false) {
      verify_case->expectations.push_back(
          {at,
           device_count,
           stream_count,
           0,
           false,
           require_topology_change});
      return *this;
    }

    AtBuilder& expect_with_native(std::uint32_t device_count,
                                  std::uint32_t stream_count,
                                  std::uint32_t acquisition_session_count,
                                  bool expect_acquisition_session,
                                  bool require_topology_change = false) {
      verify_case->expectations.push_back(
          {at,
           device_count,
           stream_count,
           acquisition_session_count,
           expect_acquisition_session,
           require_topology_change});
      return *this;
    }
  };

  std::string name;
  VerifyCaseProviderMask provider_mask = VerifyCaseProviderMask::Any;
  std::vector<VerifyCaseEvent> events;
  std::vector<VerifyCaseExpectation> expectations;

  explicit VerifyCase(std::string name_in, VerifyCaseProviderMask providers = VerifyCaseProviderMask::Any)
      : name(std::move(name_in)), provider_mask(providers) {}

  AtBuilder at(VerifyCaseTime time) { return AtBuilder{this, time}; }
};

} // namespace cambang
