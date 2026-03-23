// src/core/core_frame_sink.h
#pragma once

// NOTE: For this FrameView stage we allow a narrow dependency from core → provider
// contract datatypes. Keep this include boundary tight and avoid spreading FrameView
// into unrelated core headers.

#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

enum class CoreVisibilityPath : uint8_t {
  NONE = 0,
  RGBA_DIRECT = 1,
  BGRA_SWIZZLED = 2,
  REJECTED_UNSUPPORTED = 3,
  REJECTED_INVALID = 4,
};

// Core-thread frame sink hook.
//
// Called by CoreDispatcher on the core thread when consuming provider-frame commands.
// The sink must ensure deterministic release semantics for the FrameView payload.
//
// For robustness and to reduce accidental double-release patterns, FrameView is passed
// by value (moved in).
class ICoreFrameSink {
public:
  virtual ~ICoreFrameSink() = default;
  virtual CoreVisibilityPath on_frame(FrameView frame) = 0;
};

} // namespace cambang
