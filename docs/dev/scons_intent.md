# CamBANG – SCons Build Intent (Working Note)

> Status: Working Note — Not Frozen  
> This document captures current build-direction intent.
> It may evolve and does not carry architectural authority.

---

## 1. Purpose

This document records the intended direction of CamBANG’s SCons integration
prior to full GDExtension / godot-cpp integration.

It exists to:

- Preserve agreed direction across development phases.
- Avoid accidental drift in toolchain or CLI conventions.
- Maintain alignment with Godot’s official ecosystem practices.

This document is not part of the frozen architecture documentation.

---

## 2. Guiding Principles

### 2.1 Mirror Godot Ecosystem Conventions

CamBANG’s SCons interface mirrors the vocabulary used by:

- godot-cpp-template
- Godot engine SCons builds

Primary arguments:

- `platform`
- `target`
- `arch`
- `precision`

Windows-specific selection (future-compatible):

- `use_mingw`
- `use_llvm`

We avoid inventing custom argument names unless strictly necessary.

---

### 2.2 No Silent Toolchain Selection

On Windows systems where both MSVC and MinGW are present:

- Toolchain choice must be explicit or clearly reported.
- No silent fallback that changes ABI expectations.

When building from Git Bash / MinGW environments:

- `use_mingw=yes` is recommended.

---

### 2.3 Local Builds Are First-Class

CamBANG must:

- Build locally without CI.
- Not depend on cloud tooling.
- Not assume internet access.

CI will be introduced later for release validation and artifact production,
but local iteration remains primary.

---

### 2.4 Future Delegation to godot-cpp

Once GDExtension integration begins:

- CamBANG will delegate toolchain logic to `godot-cpp/SConstruct`
  (as per official template pattern).
- CamBANG SConstruct should become a thin orchestration layer,
  not a compiler-policy authority.

---

## 3. Windows Release Policy (Planned)

For distributed Windows builds:

- Prefer MinGW-w64 for parity with official Godot binaries.
- MSVC may be supported as a development convenience.

This remains subject to validation during GDE integration.

---

## 4. Upstream Discrepancy Policy

If deviations from godot-cpp-template are required:

- The deviation must be documented.
- An upstream issue reference must be logged.
- The workaround must be removable.

Future document:
`docs/dev/upstream_discrepancies.md`

---

## 5. Scope

This document governs:

- Build-system direction.
- Toolchain intent.
- Compatibility philosophy.

It does not govern:

- Runtime architecture.
- Provider lifecycle.
- Core threading model.
- Resource ownership rules.

Those remain under frozen architectural documentation.