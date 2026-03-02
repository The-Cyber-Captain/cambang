#pragma once

#include <cstdint>

namespace cambang {

class SyntheticVirtualClock {
public:
  uint64_t now_ns() const noexcept { return now_ns_; }
  void advance(uint64_t dt_ns) noexcept { now_ns_ += dt_ns; }

private:
  uint64_t now_ns_ = 0;
};

} // namespace cambang
