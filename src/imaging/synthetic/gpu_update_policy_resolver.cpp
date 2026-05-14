#include "imaging/synthetic/gpu_update_policy_resolver.h"

#include <cstdlib>
#include <cstring>

namespace cambang {

namespace {

constexpr const char* kPolicyEnv = "CAMBANG_SYNTH_STREAM_GPU_UPDATE_POLICY";
constexpr const char* kExerciseEnv = "CAMBANG_EXERCISE";
constexpr const char* kExerciseNoDisplayEager = "no_display_eager";

} // namespace

const char* to_string(StreamGpuUpdatePolicy policy) noexcept {
  return policy == StreamGpuUpdatePolicy::Always ? "always" : "display_demanded";
}

StreamGpuUpdatePolicyResolution resolve_synthetic_stream_gpu_update_policy() {
  StreamGpuUpdatePolicyResolution out{};
  const char* explicit_value = std::getenv(kPolicyEnv);
  const char* exercise = std::getenv(kExerciseEnv);
  const bool exercise_no_display_eager =
      (exercise && exercise[0] != '\0' && std::strcmp(exercise, kExerciseNoDisplayEager) == 0);

  if (explicit_value && explicit_value[0] != '\0') {
    out.explicit_env_value = explicit_value;
    out.policy = (std::strcmp(explicit_value, "display_demanded") == 0)
                     ? StreamGpuUpdatePolicy::DisplayDemanded
                     : StreamGpuUpdatePolicy::Always;
    out.source = StreamGpuUpdatePolicySource::ExplicitEnv;
    if (exercise_no_display_eager && out.policy != StreamGpuUpdatePolicy::Always) {
      out.has_conflict = true;
      out.exercise_value = kExerciseNoDisplayEager;
      out.error_message =
          "configuration conflict: CAMBANG_EXERCISE=no_display_eager requires effective "
          "CAMBANG_SYNTH_STREAM_GPU_UPDATE_POLICY=always, but explicit "
          "CAMBANG_SYNTH_STREAM_GPU_UPDATE_POLICY=" + out.explicit_env_value;
    }
    return out;
  }

  if (exercise_no_display_eager) {
    out.policy = StreamGpuUpdatePolicy::Always;
    out.source = StreamGpuUpdatePolicySource::Exercise;
    out.exercise_value = kExerciseNoDisplayEager;
    return out;
  }

  out.policy = StreamGpuUpdatePolicy::DisplayDemanded;
  out.source = StreamGpuUpdatePolicySource::Default;
  return out;
}

} // namespace cambang
