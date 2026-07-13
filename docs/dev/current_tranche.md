# Current Tranche

## Camera Facts Tranche 9 - Closeout Repair

### Objective

Complete the existing camera-fact work after closeout inspection found that
provider-owned per-image facts already present in the model do not traverse the
full provider-to-result path.

This remains Tranche 9 closeout work, not a new feature tranche.

### Defect

`CaptureImageFacts` models:

- image acquisition timing;
- realized focus state;
- realized delivered-image transform.

The current implementation does not carry these facts through
`ProviderCaptureImageFacts`, validation, Core retention/resolution,
CaptureResult member storage, SyntheticProvider declarations, the existing
`camera_facts` dictionary, or the existing camera-fact verification surfaces.

### Required Work

Repair the existing path end to end:

- complete provider capture-image fact ingress and atomic validation;
- retain facts by exact capture id, device instance id, and member index;
- resolve and store complete per-member facts in CaptureResult;
- correct recently introduced camera-static-only internal names or structures
  where appropriate;
- correct the existing `camera_facts` dictionary directly, without compatibility
  aliases or a parallel fact surface;
- make SyntheticProvider declare, for every retained capture member:
    - acquisition timing from that member's actual provider-domain mark;
    - focus at infinity for the current pinhole/no-defocus model;
    - zero-degree, unmirrored, already-realized image transform;
    - `VIRTUAL_CAMERA_AUTHORED` provenance;
- retain static pose fallback; do not fabricate per-image pose;
- update governing camera-fact and provider-compliance documentation.

Keep provider image acquisition timing distinct from Capture Date-Time,
geolocation, Core admission time, Core lifecycle timing, and other members'
timing. Keep sensor orientation distinct from realized image transform.

### Verification

Repair and strengthen the existing verification surfaces rather than adding a
parallel verifier family or new scene, including:

- provider camera-fact ingress coverage;
- Core camera-fact resolution/result-path coverage;
- SyntheticProvider reference camera-fact coverage;
- Scene 569 and any affected fixtures or dictionary expectations.

Coverage must prove:

- valid complete records are accepted and malformed records rejected atomically;
- absence and replacement semantics remain correct;
- no fact leakage occurs across captures, devices, rig members, or image members;
- every SyntheticProvider capture member exposes the expected facts and origin;
- existing static facts, calibration resolution, pose fallback, and per-image
  pose precedence remain correct.

Tests may be corrected where they encode the incomplete implementation, but not
weakened merely to pass.

### Constraints

No:

- geolocation or Capture Date-Time behaviour changes;
- capture-admission ownership changes;
- stream-result camera facts;
- new public methods, wrappers, setters, or override APIs;
- CameraSpec or ImagingSpec redesign;
- synthetic motion/scenario feature work;
- platform-provider implementation;
- compatibility support for the incomplete dictionary.

### Validation and Completion

Run all validation required by `AGENTS.md` and all affected existing checks,
including:

- maintainer-tools build;
- focused camera-fact checks;
- full `core_spine_smoke.exe --verbose`;
- full `provider_compliance_verify.exe --verbose`;
- Windows GDE build;
- Android arm64 GDE build;
- Scene 569 through the sanctioned PowerShell runner.

Record exact commands and results.

Tranche 9 closes only when the complete provider-owned capture-image fact path
is implemented, exposed through the existing `camera_facts` surface, verified
for exact identity/provenance/absence semantics, and aligned across source,
tests, bindings, documentation, and agent guidance.
