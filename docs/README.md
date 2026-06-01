# CamBANG

CamBANG is a Godot 4.5+ GDExtension project for camera-provider experimentation,
synthetic provider verification, runtime diagnostics, and snapshot-backed status
surfacing.

The project is currently in active development. The current repository contains:

- platform-independent Core runtime and snapshot publication code
- provider contract definitions under `src/imaging/api/`
- stub and synthetic providers for deterministic verification
- a Windows Media Foundation provider path quarantined as `windows_mediafoundation(dev accelerator)`
- Godot-facing GDExtension bindings under `src/godot/`
- snapshot schema v1 under `schema/state_snapshot/v1/`
- Godot StatusPanel harnesses and fixtures under `tests/cambang_gde/`

## Current platform status

The current SCons entrypoint explicitly rejects `platform=android`. Android /
Camera2 support is therefore future work, not the current build path.

Current provider work should follow the provider contract and architecture docs
rather than platform-specific assumptions from older drafts. The MF dev
accelerator is not the Release Windows provider and is not the provider
foundation to copy.

## Documentation map

Start with:

- `docs/INDEX.md` — documentation authority and reading order
- `docs/provider_architecture.md` — provider/Core contract boundaries
- `docs/core_runtime_model.md` — Core runtime authority model
- `docs/state_snapshot.md` — human-readable snapshot schema and truth model
- `schema/state_snapshot/v1/state_snapshot_schema.json` — machine-readable schema

Development-stage notes live under `docs/dev/` and are subordinate to the
canonical documents listed in `docs/INDEX.md`.

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