#pragma once

#include <cstdint>

#include "core/core_mailbox.h"
#include "core/core_thread.h"
#include "provider/icamera_provider.h"

namespace cambang {

    // ProviderCallbackIngress is the ONLY allowed implementation point where provider
    // callbacks cross into core execution.
    //
    // Invariants:
    // - Provider threads MUST NOT touch core state directly.
    // - Every callback is marshalled into a CoreCommand and posted onto CoreThread.
    // - No business logic exists here; this is a transport adapter only.
    class ProviderCallbackIngress final : public IProviderCallbacks {
    public:
        ProviderCallbackIngress(CoreThread* core_thread);
        ~ProviderCallbackIngress() override = default;

        ProviderCallbackIngress(const ProviderCallbackIngress&) = delete;
        ProviderCallbackIngress& operator=(const ProviderCallbackIngress&) = delete;

        // IProviderCallbacks
        void on_device_opened(uint64_t device_instance_id) override;
        void on_device_closed(uint64_t device_instance_id) override;

        void on_stream_created(uint64_t stream_id) override;
        void on_stream_destroyed(uint64_t stream_id) override;
        void on_stream_started(uint64_t stream_id) override;
        void on_stream_stopped(uint64_t stream_id, ProviderError error_or_ok) override;

        void on_capture_started(uint64_t capture_id) override;
        void on_capture_completed(uint64_t capture_id) override;
        void on_capture_failed(uint64_t capture_id, ProviderError error) override;

        void on_frame(const FrameView& frame) override;

        void on_device_error(uint64_t device_instance_id, ProviderError error) override;
        void on_stream_error(uint64_t stream_id, ProviderError error) override;

        void on_native_object_created(const NativeObjectCreateInfo& info) override;
        void on_native_object_destroyed(const NativeObjectDestroyInfo& info) override;

    private:
        void post_command(CoreCommand cmd);

        CoreThread* core_thread_ = nullptr; // non-owning
    };

} // namespace cambang