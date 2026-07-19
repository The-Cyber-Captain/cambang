# CamBANG

CamBANG is a Godot 4.5+ GDExtension project for camera-provider experimentation,
synthetic provider verification, runtime diagnostics, and snapshot-backed status
surfacing.

The project is currently in active development. The current repository contains:

- platform-independent Core runtime and snapshot publication code
- provider contract definitions under `src/imaging/api/`
- stub and synthetic providers for deterministic verification
- Godot-facing GDExtension bindings under `src/godot/`
- snapshot schema v1 under `schema/state_snapshot/v1/`
- ADC camera-description schema v2 under `schema/adc/camera_description/v2/`
- Godot StatusPanel harnesses and fixtures under `tests/cambang_gde/`

## Current platform status

The current SCons entrypoint supports an Android GDE build path. On this source
snapshot, `platform=android` builds the Android-targeted GDExtension artifact
through the pinned Android NDK toolchain when the local SDK/NDK is available.

Android platform-backed provider work is not yet compiled into that artifact:
the current `android_camera2` provider family reports `not_compiled`, and the
Android GDE build therefore remains synthetic-only rather than a platform-backed
runtime path.

Current provider work should follow the provider contract and architecture docs
rather than platform-specific assumptions from older drafts. Windows
platform-backed work targets the `windows_winrt` family (implemented by
`WinrtCameraProvider` over the Media Foundation capture stack; see
`docs/dev/build_and_scaffolding.md` §6) rather than inferring behavior from
synthetic or stub implementation details.

## Documentation map

Start with:

- `docs/INDEX.md` - documentation authority and reading order
- `docs/provider_architecture.md` - provider/Core contract boundaries
- `docs/core_runtime_model.md` - Core runtime authority model
- `docs/camera_fact_model.md` - canonical CamBANG source-neutral camera-fact architecture
- `docs/adc_camera_description_v2.md` - provisionally hosted human-readable ADC
  v2 camera-description contract targeted by CamBANG
- `docs/state_snapshot.md` - human-readable snapshot schema and truth model
- `schema/state_snapshot/v1/state_snapshot_schema.json` - machine-readable snapshot schema

Machine-readable external contract material currently includes:

- `schema/adc/camera_description/v2/adc_camera_description_schema.json` -
  provisionally hosted ADC v2 schema targeted by CamBANG

Development-stage notes live under `docs/dev/` and are subordinate to the
canonical CamBANG documents listed in `docs/INDEX.md`.

## Build and validation

Build and validation are SCons-based. See:

- `docs/dev/build_and_scaffolding.md`
- `docs/dev/maintainer_tools.md`
- `tests/cambang_gde/README.md`

Treat verifier fixtures as authored verification artifacts. Do not weaken or
mutate fixtures merely to match current output; classify any mismatch first.

## Licensing

Code: The Unlicense.

See `docs/THIRD_PARTY_NOTICES.md` for third-party notices.

### Support me! 🥛🍞

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/L4L81SGS9W)
