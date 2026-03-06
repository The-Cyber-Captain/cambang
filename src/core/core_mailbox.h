#pragma once

#include <cstdint>
#include <variant>
#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

// -----------------------------------------------------------------------------
// Core Mailbox
//
// Defines the closed set of commands that may be executed on the core thread.
// All external stimuli must be translated into one of these commands before
// entering the core thread.
//
// No provider headers.
// No Godot headers.
// No core state.
// Pure data transport.
// -----------------------------------------------------------------------------

// Enumerates all core command types.
// This must remain explicit and closed (no open-ended polymorphism).
enum class CoreCommandType : uint8_t {
  INVALID = 0,

  // Provider lifecycle
  PROVIDER_DEVICE_OPENED,
  PROVIDER_DEVICE_CLOSED,

  PROVIDER_STREAM_CREATED,
  PROVIDER_STREAM_DESTROYED,
  PROVIDER_STREAM_STARTED,
  PROVIDER_STREAM_STOPPED,

  PROVIDER_CAPTURE_STARTED,
  PROVIDER_CAPTURE_COMPLETED,
  PROVIDER_CAPTURE_FAILED,

  PROVIDER_FRAME,

  PROVIDER_DEVICE_ERROR,
  PROVIDER_STREAM_ERROR,

  PROVIDER_NATIVE_OBJECT_CREATED,
  PROVIDER_NATIVE_OBJECT_DESTROYED,

  // Timer tick (internal)
  TIMER_TICK
};

// ---------------- Payload Definitions ----------------

// Keep payloads minimal and strictly typed.
// These mirror the provider callback parameters but do not include provider headers.

struct CmdProviderDeviceOpened {
  uint64_t device_instance_id = 0;
};

struct CmdProviderDeviceClosed {
  uint64_t device_instance_id = 0;
};

struct CmdProviderStreamCreated {
  uint64_t stream_id = 0;
};

struct CmdProviderStreamDestroyed {
  uint64_t stream_id = 0;
};

struct CmdProviderStreamStarted {
  uint64_t stream_id = 0;
};

struct CmdProviderStreamStopped {
  uint64_t stream_id = 0;
  uint32_t error_code = 0; // ProviderError as uint32_t
};

struct CmdProviderCaptureStarted {
  uint64_t capture_id = 0;
};

struct CmdProviderCaptureCompleted {
  uint64_t capture_id = 0;
};

struct CmdProviderCaptureFailed {
  uint64_t capture_id = 0;
  uint32_t error_code = 0;
};

struct CmdProviderFrame {
  FrameView frame; // By value: preserves release hook and all metadata.
};
  
struct CmdProviderDeviceError {
  uint64_t device_instance_id = 0;
  uint32_t error_code = 0;
};

struct CmdProviderStreamError {
  uint64_t stream_id = 0;
  uint32_t error_code = 0;
};

struct CmdProviderNativeObjectCreated {
  uint64_t native_id = 0;
  uint32_t type = 0;
  uint64_t root_id = 0;
  uint64_t owner_device_instance_id = 0;
  uint64_t owner_stream_id = 0;
  uint64_t bytes_allocated = 0;
  uint32_t buffers_in_use = 0;
  uint64_t created_ns = 0;
};

struct CmdProviderNativeObjectDestroyed {
  uint64_t native_id = 0;
  uint64_t destroyed_ns = 0;
};

struct CmdTimerTick {};

// Variant containing all possible payloads.
using CoreCommandPayload = std::variant<
  CmdProviderDeviceOpened,
  CmdProviderDeviceClosed,
  CmdProviderStreamCreated,
  CmdProviderStreamDestroyed,
  CmdProviderStreamStarted,
  CmdProviderStreamStopped,
  CmdProviderCaptureStarted,
  CmdProviderCaptureCompleted,
  CmdProviderCaptureFailed,
  CmdProviderFrame,
  CmdProviderDeviceError,
  CmdProviderStreamError,
  CmdProviderNativeObjectCreated,
  CmdProviderNativeObjectDestroyed,
  CmdTimerTick
>;

// Wrapper command object.
struct CoreCommand {
  CoreCommandType type = CoreCommandType::INVALID;
  CoreCommandPayload payload;
};

} // namespace cambang
