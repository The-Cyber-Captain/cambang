#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "Pattern render bench: build with -DCAMBANG_INTERNAL_SMOKE=1 (via SCons: smoke=1)."
#endif

#include "pixels/pattern/active_pattern_config.h"
#include "pixels/pattern/cpu_packed_pattern_renderer.h"
#include "pixels/pattern/pattern_render_target.h"

using namespace cambang;

namespace {

struct Options {
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t frames = 2000;
  bool overlay = true;
  bool bgra = false;

  // Pattern selection (string token).
  std::string pattern = "xy_xor";

  // Optional overrides.
  uint32_t seed = 0;
  uint32_t checker_size = 16;
  uint8_t rgba[4] = {0, 0, 0, 255};
  bool rgba_set = false;
  bool checker_set = false;
  bool seed_set = false;
};

static void usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0
      << " [--pattern=<name>] [--seed=N] [--checker_size=PX] [--rgba=R,G,B[,A]]\n"
      << "             [--w=W] [--h=H] [--frames=N] [--no_overlay] [--bgra]\n"
      << "\n"
      << "Patterns:\n";
  for (uint32_t i = 0; i < pattern_preset_count(); ++i) {
    const auto& e = kPatternPresetRegistry[i];
    std::cerr << "  " << e.name;
    if (e.display_name) std::cerr << "  (" << e.display_name << ")";
    std::cerr << "\n";
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

static bool parse_rgba(const std::string& s, uint8_t out[4]) {
  // Accept "R,G,B" or "R,G,B,A" where components are 0..255.
  uint32_t comps[4] = {0, 0, 0, 255};
  int count = 0;

  size_t start = 0;
  while (start < s.size() && count < 4) {
    size_t comma = s.find(',', start);
    std::string tok = (comma == std::string::npos) ? s.substr(start) : s.substr(start, comma - start);
    if (tok.empty()) return false;

    uint32_t v = 0;
    if (!parse_u32(tok, v) || v > 255) return false;
    comps[count++] = v;

    if (comma == std::string::npos) break;
    start = comma + 1;
  }

  if (count != 3 && count != 4) return false;

  out[0] = static_cast<uint8_t>(comps[0]);
  out[1] = static_cast<uint8_t>(comps[1]);
  out[2] = static_cast<uint8_t>(comps[2]);
  out[3] = static_cast<uint8_t>(comps[3]);
  return true;
}

static bool parse_opts(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(argv[0]);
      return false;
    }
    if (starts_with(a, "--pattern=")) {
      opt.pattern = a.substr(std::strlen("--pattern="));
      continue;
    }
    if (starts_with(a, "--seed=")) {
      if (!parse_u32(a.substr(std::strlen("--seed=")), opt.seed)) {
        std::cerr << "Invalid --seed\n";
        return false;
      }
      opt.seed_set = true;
      continue;
    }
    if (starts_with(a, "--checker_size=")) {
      if (!parse_u32(a.substr(std::strlen("--checker_size=")), opt.checker_size) || opt.checker_size == 0) {
        std::cerr << "Invalid --checker_size\n";
        return false;
      }
      opt.checker_set = true;
      continue;
    }
    if (starts_with(a, "--rgba=")) {
      if (!parse_rgba(a.substr(std::strlen("--rgba=")), opt.rgba)) {
        std::cerr << "Invalid --rgba (expected R,G,B[,A] with 0..255)\n";
        return false;
      }
      opt.rgba_set = true;
      continue;
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

  const auto* preset_info = find_pattern_preset(std::string_view(opt.pattern));
  if (!preset_info) {
    std::cerr << "Unknown --pattern: " << opt.pattern << "\n";
    usage(argv[0]);
    return 2;
  }

  ActivePatternConfig cfg{};
  cfg.preset = preset_info->preset;
  cfg.seed = opt.seed;
  cfg.overlay_frame_index_offsets = opt.overlay;
  cfg.overlay_moving_bar = opt.overlay;

  if (cfg.preset == PatternPreset::Solid) {
    if (opt.rgba_set) {
      cfg.solid_r = opt.rgba[0];
      cfg.solid_g = opt.rgba[1];
      cfg.solid_b = opt.rgba[2];
      cfg.solid_a = opt.rgba[3];
    } else {
      // Default solid is opaque black (cfg defaults).
    }
  }

  if (cfg.preset == PatternPreset::Checker) {
    if (opt.checker_set) {
      cfg.checker_size_px = opt.checker_size;
    }
  }

  const auto fmt = opt.bgra ? PatternSpec::PackedFormat::BGRA8 : PatternSpec::PackedFormat::RGBA8;
  PatternSpec spec = to_pattern_spec(cfg, opt.width, opt.height, fmt);

  const uint32_t stride = spec.width * 4u;
  const size_t buf_bytes = static_cast<size_t>(stride) * static_cast<size_t>(spec.height);
  std::vector<uint8_t> buf(buf_bytes);

  PatternRenderTarget dst{};
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
      << "pattern: " << preset_info->name << "\n"
      << "resolution: " << spec.width << "x" << spec.height << "\n"
      << "format: " << (opt.bgra ? "BGRA8" : "RGBA8") << "\n"
      << "frames: " << opt.frames << "\n"
      << "time: " << secs << "s\n"
      << "fps: " << fps << "\n"
      << "bytes/frame: " << static_cast<uint64_t>(buf_bytes) << "\n"
      << "approx bandwidth: " << gbps << " GB/s\n";

  return 0;
}
