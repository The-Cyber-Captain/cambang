#pragma once

#include "pixels/pattern/pattern_render_target.h"
#include "pixels/pattern/pattern_spec.h"

namespace cambang {

class IPatternRenderer {
public:
  virtual ~IPatternRenderer() = default;

  // Optional; render_into may also reconfigure lazily.
  virtual void configure(const PatternSpec& spec) = 0;

  // Render into caller-provided target.
  //
  // Implementations must not allocate per frame.
  virtual void render_into(
      const PatternSpec& spec,
      const PatternRenderTarget& dst,
      const PatternOverlayData& overlay) = 0;
};

} // namespace cambang
