cmake_minimum_required(VERSION 3.16)

# ------------------------------------------------------------------------------
# CamBANG (IDE-only CMake project)
#
# This CMake setup exists ONLY to provide CLion (and similar IDEs) with:
#  - code indexing
#  - autocomplete
#  - basic diagnostics
#
# It is NOT the authoritative build system.
# The authoritative build will be SCons (Godot GDExtension workflow).
# ------------------------------------------------------------------------------

project(cambang_ide LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# An INTERFACE library is perfect for header-heavy early scaffolding:
# it creates a "target" CLion can attach include paths / compile features to,
# without requiring real linkable sources yet.
add_library(cambang_headers INTERFACE)

# Treat /src as the include root so you can do:
#   #include "provider/icamera_provider.h"
#   #include "provider/provider_contract_datatypes.h"
target_include_directories(cambang_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(cambang_headers INTERFACE cxx_std_17)

# Optional: enable common warnings for IDE diagnostics (non-authoritative).
# CLion will still surface these while editing.
if (MSVC)
    target_compile_options(cambang_headers INTERFACE /W4)
else()
    target_compile_options(cambang_headers INTERFACE -Wall -Wextra -Wpedantic)
endif()

# Add a dummy "translation unit" so CLion has something concrete to compile/index.
# This avoids edge cases where only-header projects get poorer inspection.
add_library(cambang_ide STATIC
    ide/ide_dummy.cpp
    src/provider/icamera_provider.h
    src/provider/provider_contract_datatypes.h
    src/provider/provider_error_string.h
    src/provider/provider_error_string.cpp
        src/provider/stub/stub_camera_provider.h
        src/provider/stub/stub_camera_provider.cpp
        src/core/core_thread.h
        src/core/core_thread.cpp
        src/core/core_mailbox.h
        src/core/core_dispatcher.h
        src/core/core_dispatcher.cpp
        src/core/core_snapshot.h
        src/core/core_publisher.h
        src/core/core_publisher_buffer.h
        src/core/core_stream_registry.h
        src/core/core_stream_registry.cpp
        src/core/core_runtime.h
        src/core/core_runtime.cpp
        src/core/provider_callback_ingress.h
        src/core/provider_callback_ingress.cpp
        src/godot/module_init.cpp
        src/godot/dev/cambang_dev_node.cpp
        src/godot/dev/cambang_dev_node.h
)

target_link_libraries(cambang_ide PRIVATE cambang_headers)

# Smoke test executable (IDE-only). Proves frame lifecycle end-to-end.
add_executable(cambang_core_spine_smoke
    ide/core_spine_smoke.cpp
)

target_link_libraries(cambang_core_spine_smoke PRIVATE cambang_ide cambang_headers)
