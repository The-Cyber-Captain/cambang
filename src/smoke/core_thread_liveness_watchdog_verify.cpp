// Death test for CoreRuntime's enforcement of the provider prompt/bounded
// contract through CoreRuntime::check_core_thread_liveness().
//
// Normal invocation is the supervising verifier: it launches this executable
// in an internal child mode, captures its output, enforces a bounded deadline,
// and prints the final PASS/FAIL verdict. The child deliberately induces a
// provider call that blocks beyond provider_architecture.md Section 8.1's
// prompt/bounded contract, so CAMBANG_INTERNAL_SMOKE's watchdog abort path
// must terminate only that child process.

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "core_thread_liveness_watchdog_verify: build through the repo SCons maintainer_tools alias so CAMBANG_INTERNAL_SMOKE=1 is defined."
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  // windows.h exposes ERROR as a macro; CamBANG snapshot enums use the
  // ordinary scoped enumerator name and must not be macro-substituted.
  #ifdef ERROR
    #undef ERROR
  #endif
#else
  #include <cerrno>
  #include <csignal>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

#include "core/core_runtime.h"
#include "core/state_snapshot_buffer.h"
#include "imaging/api/icamera_provider.h"
#include "imaging/stub/provider.h"

namespace cambang {
namespace {

// Forwards every ICameraProvider call to a real StubProvider (StubProvider
// is `final`, so composition rather than inheritance), except
// trigger_capture(), which sleeps well past both CoreRuntime's 2s
// caller-facing future::wait_for() bound and the 5s core-thread staleness
// threshold before forwarding. This deliberately violates
// provider_architecture.md Section 8.1's "prompt, bounded" contract.
class HangingCaptureProvider final : public ICameraProvider {
public:
  explicit HangingCaptureProvider(std::chrono::milliseconds hang_for)
      : inner_(std::make_unique<StubProvider>()), hang_for_(hang_for) {}

  const char* provider_name() const override { return inner_->provider_name(); }
  ProviderKind provider_kind() const noexcept override { return inner_->provider_kind(); }
  StreamTemplate stream_template() const override { return inner_->stream_template(); }
  CaptureTemplate capture_template() const override { return inner_->capture_template(); }
  bool supports_stream_picture_updates() const noexcept override {
    return inner_->supports_stream_picture_updates();
  }
  bool supports_capture_picture_updates() const noexcept override {
    return inner_->supports_capture_picture_updates();
  }
  bool supports_multi_image_still_sequence() const noexcept override {
    return inner_->supports_multi_image_still_sequence();
  }
  ProducerBackingCapabilities stream_backing_capabilities(
      const CaptureProfile& profile, const PictureConfig& picture) const noexcept override {
    return inner_->stream_backing_capabilities(profile, picture);
  }
  ProducerBackingCapabilities capture_backing_capabilities(
      const CaptureRequest& req) const noexcept override {
    return inner_->capture_backing_capabilities(req);
  }
  uint64_t stream_backing_plan_evaluation_settle_delay_ns() const noexcept override {
    return inner_->stream_backing_plan_evaluation_settle_delay_ns();
  }
  uint64_t capture_backing_plan_evaluation_settle_delay_ns() const noexcept override {
    return inner_->capture_backing_plan_evaluation_settle_delay_ns();
  }

  ProviderResult initialize(IProviderCallbacks* callbacks) override {
    return inner_->initialize(callbacks);
  }
  ProviderResult enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) override {
    return inner_->enumerate_endpoints(out_endpoints);
  }
  ProviderResult open_device(
      const std::string& hardware_id, uint64_t device_instance_id, uint64_t root_id) override {
    return inner_->open_device(hardware_id, device_instance_id, root_id);
  }
  ProviderResult close_device(uint64_t device_instance_id) override {
    return inner_->close_device(device_instance_id);
  }
  ProviderResult create_stream(const StreamRequest& req) override {
    return inner_->create_stream(req);
  }
  ProviderResult destroy_stream(uint64_t stream_id) override {
    return inner_->destroy_stream(stream_id);
  }
  ProviderResult start_stream(
      uint64_t stream_id, const CaptureProfile& profile, const PictureConfig& picture) override {
    return inner_->start_stream(stream_id, profile, picture);
  }
  ProviderResult stop_stream(uint64_t stream_id) override { return inner_->stop_stream(stream_id); }
  ProviderResult update_stream_retained_production_plan(
      uint64_t stream_id, CoreRetainedProductionPlan requested_retained_plan) override {
    return inner_->update_stream_retained_production_plan(stream_id, requested_retained_plan);
  }
  ProviderResult set_stream_picture_config(uint64_t stream_id, const PictureConfig& picture) override {
    return inner_->set_stream_picture_config(stream_id, picture);
  }
  ProviderResult set_capture_picture_config(
      uint64_t device_instance_id, const PictureConfig& picture) override {
    return inner_->set_capture_picture_config(device_instance_id, picture);
  }
  ProviderResult sync_capture_parent_priming(const CaptureRequest& req) override {
    return inner_->sync_capture_parent_priming(req);
  }
  ProviderResult release_capture_parent_priming(uint64_t device_instance_id) override {
    return inner_->release_capture_parent_priming(device_instance_id);
  }

  ProviderResult trigger_capture(const CaptureRequest& req) override {
    std::fprintf(stderr,
                 "[core_thread_liveness_watchdog_verify] HangingCaptureProvider::trigger_capture "
                 "sleeping for %llds to deliberately violate the prompt/bounded contract...\n",
                 static_cast<long long>(
                     std::chrono::duration_cast<std::chrono::seconds>(hang_for_).count()));
    std::fflush(stderr);
    std::this_thread::sleep_for(hang_for_);
    return inner_->trigger_capture(req);
  }

  ProviderResult abort_capture(uint64_t capture_id) override {
    return inner_->abort_capture(capture_id);
  }

  ProviderResult apply_camera_spec_patch(
      const std::string& hardware_id,
      uint64_t new_camera_spec_version,
      SpecPatchView patch) override {
    return inner_->apply_camera_spec_patch(hardware_id, new_camera_spec_version, patch);
  }
  ProviderResult apply_imaging_spec_patch(
      uint64_t new_imaging_spec_version, SpecPatchView patch) override {
    return inner_->apply_imaging_spec_patch(new_imaging_spec_version, patch);
  }

  ProviderResult shutdown() override { return inner_->shutdown(); }

private:
  std::unique_ptr<ICameraProvider> inner_;
  std::chrono::milliseconds hang_for_;
};

bool wait_until(const std::function<bool()>& pred, int max_iters, int sleep_ms) {
  for (int i = 0; i < max_iters; ++i) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  return false;
}

constexpr const char* kDeathChildArgument = "--death-child";
constexpr auto kDeathChildTimeout = std::chrono::seconds(15);

struct ChildRunResult final {
  bool launched = false;
  bool timed_out = false;
  bool terminated_by_signal = false;
  int signal_number = 0;
  uint64_t exit_code = 0;
  std::string output;
  std::string launch_error;
};

#if defined(_WIN32)

std::string windows_error_message(DWORD error) {
  char* message = nullptr;
  const DWORD length = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr,
      error,
      0,
      reinterpret_cast<char*>(&message),
      0,
      nullptr);
  std::string result = length != 0 && message ? std::string(message, length)
                                               : "Windows error " + std::to_string(error);
  if (message) {
    LocalFree(message);
  }
  return result;
}

std::string current_executable_path() {
  std::vector<char> buffer(1024);
  for (;;) {
    const DWORD length = GetModuleFileNameA(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return {};
    }
    if (length < buffer.size() - 1) {
      return std::string(buffer.data(), length);
    }
    buffer.resize(buffer.size() * 2);
  }
}

ChildRunResult run_death_child_process(const char*) {
  ChildRunResult result;
  const std::string executable = current_executable_path();
  if (executable.empty()) {
    result.launch_error = "GetModuleFileNameA failed";
    return result;
  }

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
    result.launch_error = windows_error_message(GetLastError());
    return result;
  }
  if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
    result.launch_error = windows_error_message(GetLastError());
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return result;
  }

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = write_pipe;
  startup.hStdError = write_pipe;
  PROCESS_INFORMATION process{};
  std::string command_line = "\"" + executable + "\" " + kDeathChildArgument;

  const BOOL created = CreateProcessA(
      executable.c_str(),
      command_line.data(),
      nullptr,
      nullptr,
      TRUE,
      CREATE_NO_WINDOW,
      nullptr,
      nullptr,
      &startup,
      &process);
  CloseHandle(write_pipe);
  if (!created) {
    result.launch_error = windows_error_message(GetLastError());
    CloseHandle(read_pipe);
    return result;
  }
  result.launched = true;

  const DWORD wait_ms = static_cast<DWORD>(
      std::chrono::duration_cast<std::chrono::milliseconds>(kDeathChildTimeout).count());
  const DWORD wait_result = WaitForSingleObject(process.hProcess, wait_ms);
  if (wait_result == WAIT_TIMEOUT) {
    result.timed_out = true;
    (void)TerminateProcess(process.hProcess, 124);
    (void)WaitForSingleObject(process.hProcess, 5000);
  } else if (wait_result == WAIT_FAILED) {
    result.launch_error = windows_error_message(GetLastError());
    (void)TerminateProcess(process.hProcess, 125);
    (void)WaitForSingleObject(process.hProcess, 5000);
  }

  DWORD exit_code = 0;
  if (GetExitCodeProcess(process.hProcess, &exit_code)) {
    result.exit_code = static_cast<uint64_t>(exit_code);
  }

  char chunk[4096];
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(read_pipe, chunk, sizeof(chunk), &read, nullptr) || read == 0) {
      break;
    }
    result.output.append(chunk, read);
  }

  CloseHandle(read_pipe);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return result;
}

#else

ChildRunResult run_death_child_process(const char* argv0) {
  ChildRunResult result;
  int output_pipe[2]{};
  if (pipe(output_pipe) != 0) {
    result.launch_error = std::strerror(errno);
    return result;
  }

  const pid_t child = fork();
  if (child < 0) {
    result.launch_error = std::strerror(errno);
    close(output_pipe[0]);
    close(output_pipe[1]);
    return result;
  }
  if (child == 0) {
    (void)dup2(output_pipe[1], STDOUT_FILENO);
    (void)dup2(output_pipe[1], STDERR_FILENO);
    close(output_pipe[0]);
    close(output_pipe[1]);
    execlp(argv0, argv0, kDeathChildArgument, static_cast<char*>(nullptr));
    _exit(127);
  }

  result.launched = true;
  close(output_pipe[1]);
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + kDeathChildTimeout;
  for (;;) {
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      break;
    }
    if (waited < 0) {
      if (errno == EINTR) {
        continue;
      }
      result.launch_error = std::strerror(errno);
      (void)kill(child, SIGKILL);
      (void)waitpid(child, &status, 0);
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      result.timed_out = true;
      (void)kill(child, SIGKILL);
      (void)waitpid(child, &status, 0);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (WIFSIGNALED(status)) {
    result.terminated_by_signal = true;
    result.signal_number = WTERMSIG(status);
    result.exit_code = static_cast<uint64_t>(128 + result.signal_number);
  } else if (WIFEXITED(status)) {
    result.exit_code = static_cast<uint64_t>(WEXITSTATUS(status));
  }

  char chunk[4096];
  for (;;) {
    const ssize_t read_count = read(output_pipe[0], chunk, sizeof(chunk));
    if (read_count > 0) {
      result.output.append(chunk, static_cast<size_t>(read_count));
      continue;
    }
    if (read_count < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  close(output_pipe[0]);
  return result;
}

#endif

bool child_terminated_via_expected_abort(const ChildRunResult& result) {
#if defined(_WIN32)
  // Windows C runtimes report abort with different nonzero process codes.
  // Controlled verifier/setup failures return 1, so require neither success
  // nor that controlled failure after observing the watchdog's own log line.
  return result.exit_code != 0 && result.exit_code != 1;
#else
  return result.terminated_by_signal && result.signal_number == SIGABRT;
#endif
}

} // namespace
} // namespace cambang

int run_death_child() {
  using namespace cambang;

  CoreRuntime runtime;
  StateSnapshotBuffer buffer;
  runtime.set_snapshot_publisher(&buffer);

  if (!runtime.start()) {
    std::fprintf(stderr, "FAIL: runtime start failed\n");
    return 1;
  }
  if (!wait_until([&]() { return runtime.state_copy() == CoreRuntimeState::LIVE; }, 500, 5)) {
    std::fprintf(stderr, "FAIL: timed out waiting for runtime LIVE\n");
    runtime.stop();
    return 1;
  }

  // Sleep comfortably past the synchronous command's initial 2s cancellation
  // window and the 5s core-thread staleness threshold.
  auto provider = std::make_unique<HangingCaptureProvider>(std::chrono::seconds(8));

  if (!provider->initialize(runtime.provider_callbacks()).ok()) {
    std::fprintf(stderr, "FAIL: provider initialize failed\n");
    runtime.stop();
    return 1;
  }

  std::vector<CameraEndpoint> endpoints;
  if (!provider->enumerate_endpoints(endpoints).ok() || endpoints.empty()) {
    std::fprintf(stderr, "FAIL: enumerate_endpoints failed\n");
    (void)provider->shutdown();
    runtime.stop();
    return 1;
  }

  runtime.attach_provider(provider.get());

  constexpr uint64_t kDeviceId = 100;
  constexpr uint64_t kRootId = 900;
  if (runtime.retain_device_identity(kDeviceId, endpoints.front().hardware_id) !=
      CoreThread::PostResult::Enqueued) {
    std::fprintf(stderr, "FAIL: retain_device_identity admission failed\n");
    runtime.attach_provider(nullptr);
    runtime.stop();
    return 1;
  }
  if (!provider->open_device(endpoints.front().hardware_id, kDeviceId, kRootId).ok()) {
    std::fprintf(stderr, "FAIL: open_device failed\n");
    runtime.attach_provider(nullptr);
    runtime.stop();
    return 1;
  }
  if (!wait_until(
          [&]() {
            auto snap = buffer.snapshot_copy();
            if (!snap) {
              return false;
            }
            for (const auto& d : snap->devices) {
              if (d.instance_id == kDeviceId) {
                return true;
              }
            }
            return false;
          },
          500,
          5)) {
    std::fprintf(stderr, "FAIL: timed out waiting for device open publish\n");
    runtime.attach_provider(nullptr);
    runtime.stop();
    return 1;
  }

  std::fprintf(stderr,
               "[core_thread_liveness_watchdog_verify] triggering device capture; expect the "
               "core thread to wedge inside HangingCaptureProvider::trigger_capture and the "
               "watchdog to abort this process within a few seconds of that call reaching the "
               "provider.\n");
  std::fflush(stderr);

  // Once the command begins, truthful synchronous completion requires this
  // call to remain blocked until the provider returns. Its wait path must poll
  // check_core_thread_liveness() itself because the same caller cannot also
  // run the ordinary Godot-tick poll while blocked here. The watchdog is
  // expected to abort this process at roughly the 5s stale-task threshold.
  uint64_t capture_id = 1;
  (void)runtime.try_trigger_device_capture_with_capture_id_for_server(kDeviceId, capture_id);

  std::fprintf(stderr,
               "FAIL: the blocked synchronous wait returned without the watchdog aborting; "
               "the prompt/bounded contract enforcement did not fire.\n");
  runtime.attach_provider(nullptr);
  runtime.stop();
  return 1;
}

int main(int argc, char** argv) {
  using namespace cambang;

  if (argc == 2 && std::strcmp(argv[1], kDeathChildArgument) == 0) {
    return run_death_child();
  }
  if (argc != 1) {
    std::fprintf(stdout,
                 "FAIL core_thread_liveness_watchdog_verify reason=invalid_arguments\n");
    return 1;
  }

  const ChildRunResult child = run_death_child_process(argv[0]);
  if (!child.output.empty()) {
    std::fwrite(child.output.data(), 1, child.output.size(), stdout);
    if (child.output.back() != '\n') {
      std::fputc('\n', stdout);
    }
    std::fflush(stdout);
  }

  const bool stale_log_seen =
      child.output.find("[CamBANG][CoreThread] stale task detected:") != std::string::npos;
  const bool expected_abort = child_terminated_via_expected_abort(child);
  if (!child.launched || child.timed_out || !child.launch_error.empty() ||
      !stale_log_seen || !expected_abort) {
    std::fprintf(
        stdout,
        "FAIL core_thread_liveness_watchdog_verify launched=%s timed_out=%s "
        "stale_task_log=%s expected_abort=%s exit_code=%llu reason=%s\n",
        child.launched ? "true" : "false",
        child.timed_out ? "true" : "false",
        stale_log_seen ? "true" : "false",
        expected_abort ? "true" : "false",
        static_cast<unsigned long long>(child.exit_code),
        child.launch_error.empty() ? "child_contract_not_satisfied" : child.launch_error.c_str());
    return 1;
  }

  std::fprintf(stdout,
               "PASS core_thread_liveness_watchdog_verify stale_task_log=true "
               "expected_abort=true exit_code=%llu\n",
               static_cast<unsigned long long>(child.exit_code));
  return 0;
}
