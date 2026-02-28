#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "Pattern render bench: build with -DCAMBANG_INTERNAL_SMOKE=1 (via SCons: smoke=1)."
#endif

#include "pixels/pattern/cpu_packed_pattern_renderer.h"

using namespace cambang;

namespace {

struct Options {
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t frames = 2000;
  bool overlay = true;
  bool bgra = false;
};

static void usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " [--w=W] [--h=H] [--frames=N] [--no_overlay] [--bgra]\n";
}

static bool starts_with(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

static bool parse_u32(const std::string& s, uint32_t& out) {
  try {
    size_t idx = 0;
    unsigned long v = std::stoul(s, &idx, 10);
    if (idx != s.size()) return false;
    out = static_cast<uint32_t>(v);
    return true;
  } catch (...) {
    return false;
  }
}

static bool parse_opts(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(argv[0]);
      return false;
    }
    if (starts_with(a, "--w=")) {
      if (!parse_u32(a.substr(4), opt.width) || opt.width == 0) {
        std::cerr << "Invalid --w\n";
        return false;
      }
      continue;
    }
    if (starts_with(a, "--h=")) {
      if (!parse_u32(a.substr(4), opt.height) || opt.height == 0) {
        std::cerr << "Invalid --h\n";
        return false;
      }
      continue;
    }
    if (starts_with(a, "--frames=")) {
      if (!parse_u32(a.substr(9), opt.frames) || opt.frames == 0) {
        std::cerr << "Invalid --frames\n";
        return false;
      }
      continue;
    }
    if (a == "--no_overlay") {
      opt.overlay = false;
      continue;
    }
    if (a == "--bgra") {
      opt.bgra = true;
      continue;
    }

    std::cerr << "Unknown arg: " << a << "\n";
    usage(argv[0]);
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_opts(argc, argv, opt)) {
    return 2;
  }

  PatternSpec spec;
  spec.width = opt.width;
  spec.height = opt.height;
  spec.format = opt.bgra ? PatternSpec::PackedFormat::BGRA8 : PatternSpec::PackedFormat::RGBA8;
  spec.base = PatternSpec::BasePattern::XY_XOR;
  spec.overlay_frame_index_offsets = opt.overlay;
  spec.overlay_moving_bar = opt.overlay;

  const uint32_t stride = spec.width * 4u;
  const size_t buf_bytes = static_cast<size_t>(stride) * static_cast<size_t>(spec.height);
  std::vector<uint8_t> buf(buf_bytes);

  PatternRenderTarget dst;
  dst.data = buf.data();
  dst.size_bytes = buf.size();
  dst.width = spec.width;
  dst.height = spec.height;
  dst.stride_bytes = stride;
  dst.format = spec.format;

  CpuPackedPatternRenderer r;
  r.configure(spec);

  // Warm-up.
  PatternOverlayData ov{};
  for (uint32_t i = 0; i < 16; ++i) {
    ov.frame_index = i;
    r.render_into(spec, dst, ov);
  }

  const auto t0 = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < opt.frames; ++i) {
    ov.frame_index = i;
    r.render_into(spec, dst, ov);
  }
  const auto t1 = std::chrono::steady_clock::now();

  const std::chrono::duration<double> dt = t1 - t0;
  const double secs = dt.count();
  const double fps = (secs > 0.0) ? (static_cast<double>(opt.frames) / secs) : 0.0;

  const double bytes_per_frame = static_cast<double>(buf_bytes);
  const double gbps = (secs > 0.0) ? ((bytes_per_frame * static_cast<double>(opt.frames)) / secs / 1e9) : 0.0;

  std::cout
      << "resolution: " << spec.width << "x" << spec.height << "\n"
      << "format: " << (opt.bgra ? "BGRA8" : "RGBA8") << "\n"
      << "frames: " << opt.frames << "\n"
      << "time: " << secs << "s\n"
      << "fps: " << fps << "\n"
      << "bytes/frame: " << static_cast<uint64_t>(buf_bytes) << "\n"
      << "approx bandwidth: " << gbps << " GB/s\n"
      << "overlay: " << (opt.overlay ? "on" : "off") << "\n";

  return 0;
}
