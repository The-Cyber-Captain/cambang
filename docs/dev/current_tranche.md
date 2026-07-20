# Current tranche

## Remove dead flattened optical-calibration and location result fact groups

Maintainer-approved (2026-07-20, in-session direction).

### Scope

The flattened `optical_calibration` and `location_attributes` result fact
groups are writer-less leftovers superseded by, respectively, the resolved
per-member `camera_facts` surface (`get_image_member(i)["camera_facts"]`) and
the capture-admission geolocation surface (`get_geolocation()`). Remove them
and everything that becomes defunct with them:

- Public methods (both `CamBANGCaptureResult` and `CamBANGStreamResult`):
  `has_optical_calibration()`, `get_optical_calibration()`,
  `get_optical_calibration_provenance()`, `has_location_attributes()`,
  `get_location_attributes()`, `get_location_attributes_provenance()` and
  their ClassDB bindings.
- Backing types and storage: `ResultOpticalCalibrationFacts`,
  `ResultOpticalCalibrationProvenance`, `ResultLocationAttributesFacts`,
  `ResultLocationAttributesProvenance` (`src/core/result_fact_types.h`),
  their fields in `CoreImageFactBundle` (`src/core/core_result_store.h`),
  any writes to those fields, and the `to_dict` converters.
- Scene 70 assertions for these accessors are replaced with absence
  assertions (`not has_method(...)`) so the removal is verification-locked.
- Documentation references
  (`docs/architecture/pixel_payload_and_result_contract.md`).

Explicitly out of scope: the live `image_properties` group (has real
writers), `capture_attributes` (also writer-less but not covered by this
approval — flagged separately), `get_geolocation()`, and the `camera_facts`
surface.

### Acceptance / validation

- Full MinGW build (maintainer tools + GDE) and MSVC Windows GDE build stay
  clean; no remaining references to the removed identifiers in src/docs.
- Native verifiers: core_spine_smoke, provider_compliance_verify,
  restart_boundary_verify, verify_case_runner --run-all,
  core_thread_liveness_watchdog_verify.
- Godot scenes 65, 66, 70, 73, 870 verdict ok (70 exercises the updated
  absence assertions).
