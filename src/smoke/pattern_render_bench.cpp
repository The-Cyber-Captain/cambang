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
#include "pixels/pattern/active_pattern_config.h"

using namespace cambang;

namespace {

struct Options {
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t frames = 2000;
  bool overlay = true;
  bool bgra = false;

  std::string pattern_name = "xy_xor";
  uint32_t seed = 0;

  bool checker_size_set = false;
  uint32_t checker_size = 16;

  bool rgba_set = false;
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t a = 255;
};

static void usage(const char* argv0) {
  size_t n = 0;
  const auto* presets = pattern_presets(&n);
  std::cerr
      << "Usage: " << argv0
      << " [--pattern=<name>] [--seed=N] [--checker_size=PX] [--rgba=R,G,B[,A]]"
      << " [--w=W] [--h=H] [--frames=N] [--no_overlay] [--bgra]\n\n"
      << "Available patterns:\n";
  for (size_t i = 0; i < n; ++i) {
    std::cerr << "  - " << presets[i].name << "\n";
  }
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

static bool parse_u8(const std::string& s, uint8_t& out) {
  uint32_t v = 0;
  if (!parse_u32(s, v) || v > 255u) {
    return false;
  }
  out = static_cast<uint8_t>(v);
  return true;
}

static bool parse_rgba(const std::string& s, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a, bool& has_a) {
  // Format: R,G,B or R,G,B,A
  size_t p0 = 0;
  size_t p1 = s.find(',', p0);
  if (p1 == std::string::npos) return false;
  size_t p2 = s.find(',', p1 + 1);
  if (p2 == std::string::npos) return false;
  size_t p3 = s.find(',', p2 + 1);

  const std::string sr = s.substr(p0, p1 - p0);
  const std::string sg = s.substr(p1 + 1, p2 - (p1 + 1));
  const std::string sb = (p3 == std::string::npos) ? s.substr(p2 + 1) : s.substr(p2 + 1, p3 - (p2 + 1));
  if (!parse_u8(sr, r) || !parse_u8(sg, g) || !parse_u8(sb, b)) {
    return false;
  }
  if (p3 == std::string::npos) {
    has_a = false;
    a = 255;
    return true;
  }
  const std::string sa = s.substr(p3 + 1);
  if (!parse_u8(sa, a)) {
    return false;
  }
  has_a = true;
  return true;
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
    if (starts_with(a, "--pattern=")) {
      opt.pattern_name = a.substr(10);
      if (opt.pattern_name.empty()) {
        std::cerr << "Invalid --pattern\n";
        return false;
      }
      continue;
    }
    if (starts_with(a, "--seed=")) {
      if (!parse_u32(a.substr(7), opt.seed)) {
        std::cerr << "Invalid --seed\n";
        return false;
      }
      continue;
    }
    if (starts_with(a, "--checker_size=")) {
      if (!parse_u32(a.substr(15), opt.checker_size) || opt.checker_size == 0) {
        std::cerr << "Invalid --checker_size\n";
        return false;
      }
      opt.checker_size_set = true;
      continue;
    }
    if (starts_with(a, "--rgba=")) {
      bool has_a = false;
      if (!parse_rgba(a.substr(7), opt.r, opt.g, opt.b, opt.a, has_a)) {
        std::cerr << "Invalid --rgba (expected R,G,B[,A])\n";
        return false;
      }
      opt.rgba_set = true;
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

  const auto* info = find_preset_info_by_name(opt.pattern_name);
  if (!info) {
    std::cerr << "Unknown --pattern: " << opt.pattern_name << "\n";
    usage(argv[0]);
    return 2;
  }

  // Validate parameter applicability using preset caps.
  if (opt.checker_size_set && ((info->caps & kCapsCheckerSize) == 0)) {
    std::cerr << "--checker_size is not supported by pattern '" << info->name << "'\n";
    return 2;
  }
  if (opt.rgba_set && ((info->caps & kCapsRgba) == 0)) {
    std::cerr << "--rgba is not supported by pattern '" << info->name << "'\n";
    return 2;
  }

  ActivePatternConfig pcfg{};
  pcfg.preset = info->preset;
  pcfg.seed = opt.seed;
  pcfg.overlay_frame_index_offsets = opt.overlay;
  pcfg.overlay_moving_bar = opt.overlay;
  if (opt.rgba_set) {
    pcfg.solid_r = opt.r;
    pcfg.solid_g = opt.g;
    pcfg.solid_b = opt.b;
    pcfg.solid_a = opt.a;
  }
  if (opt.checker_size_set) {
    pcfg.checker_size_px = opt.checker_size;
  }

  bool preset_valid = true;
  PatternSpec spec = to_pattern_spec(
      pcfg,
      opt.width,
      opt.height,
      opt.bgra ? PatternSpec::PackedFormat::BGRA8 : PatternSpec::PackedFormat::RGBA8,
      &preset_valid);
  (void)preset_valid;

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
      << "pattern: " << opt.pattern_name << "\n"
      << "seed: " << opt.seed << "\n"
      << "frames: " << opt.frames << "\n"
      << "time: " << secs << "s\n"
      << "fps: " << fps << "\n"
      << "bytes/frame: " << static_cast<uint64_t>(buf_bytes) << "\n"
      << "approx bandwidth: " << gbps << " GB/s\n"
      << "overlay: " << (opt.overlay ? "on" : "off") << "\n";

  return 0;
}
