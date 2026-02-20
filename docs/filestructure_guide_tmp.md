cambang/
  README.md
  LICENSE
  docs/
    architecture.md
    nomenclature.md
    lifecycle.md
    android_camera2_notes.md
    determinism.md
  addons/
    cambang/
      plugin.cfg
      cambang.gd        # optional: editor helpers / thin GDScript sugar
  thirdparty/
    godot-cpp/          # submodule
  src/
    cambang/
      godot_api/
        cb_camera_server_ext.cpp
        cb_camera_manager_gd.cpp
        cb_device_gd.cpp
        cb_session_gd.cpp
        cb_stream_gd.cpp
        cb_types_gd.cpp           # Variant/Dictionary adapters
      core/
        interfaces/
          ICameraProvider.hpp
          ICameraDevice.hpp
          ICaptureSession.hpp
          IFrameStream.hpp
          IClock.hpp
          ILogSink.hpp
        impl/
          CBExecutor.cpp/.hpp
          CBLifecycle.cpp/.hpp    # state machines, transition guards
          CBBufferPool.cpp/.hpp
          CBFrameQueue.cpp/.hpp
          CBSyncAssembler.cpp/.hpp
          CBFactory.cpp/.hpp
      platform/
        stub/
          StubProvider.cpp/.hpp
        godot_camerasvr/
          GodotCameraServerProvider.cpp/.hpp   # optional rewire later
        android/
          camera2/
            ACamera2Provider.cpp/.hpp
            ACamera2Device.cpp/.hpp
            ACamera2Session.cpp/.hpp
            AImageReaderStream.cpp/.hpp
            AJNI.cpp/.hpp                      # if using JNI bridge
  include/
    cambang/            # public-ish headers if needed
  test/
    core/
      test_sync_assembler.cpp
      test_state_machine.cpp
  SConstruct
  SCsub
  cambang.gdextension