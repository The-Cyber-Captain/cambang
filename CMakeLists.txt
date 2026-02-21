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
)

target_link_libraries(cambang_ide PRIVATE cambang_headers)