#pragma once

#include <string>

namespace cambang {

enum class StreamGpuUpdatePolicy {
  Always,
  DisplayDemanded,
};

enum class StreamGpuUpdatePolicySource {
  ExplicitEnv,
  Exercise,
  Default,
};

struct StreamGpuUpdatePolicyResolution {
  StreamGpuUpdatePolicy policy = StreamGpuUpdatePolicy::DisplayDemanded;
  StreamGpuUpdatePolicySource source = StreamGpuUpdatePolicySource::Default;
  bool has_conflict = false;
  std::string explicit_env_value;
  std::string exercise_value;
  std::string error_message;
};

StreamGpuUpdatePolicyResolution resolve_synthetic_stream_gpu_update_policy();
const char* to_string(StreamGpuUpdatePolicy policy) noexcept;

} // namespace cambang
