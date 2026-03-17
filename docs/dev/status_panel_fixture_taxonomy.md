# Status Panel Fixture Taxonomy

Status: Dev planning note (verification harness design)

This note proposes a fixture taxonomy and harness flow for future
`CamBANGStatusPanel` verification.

## 1) Scope and goals

The harness must test both:
- strict authoritative snapshot contract handling, and
- retained/model/projection behavior used by panel continuity logic.

It must intentionally cover valid and invalid inputs while clearly
separating:
- **expected-invalid test data** (deliberate), from
- **accidental invalidity** (fixture rot or harness bug).

---

## 2) Fixture taxonomy

We define four top-level fixture classes.

## A. Valid authoritative snapshot fixtures

Purpose:
- prove happy-path compatibility with the canonical snapshot schema
- exercise semantic edge states that are still schema-valid

Examples:
- minimal baseline snapshot (`version=0`, empty arrays where allowed)
- fully populated mixed topology (rig/device/stream/native object coverage)
- schema-valid sparse truth (`0`/empty values where runtime truth may be unknown)
- enum edge values that are valid but not commonly emitted in current scaffolding

Expected behavior:
- schema validation passes
- projector (if enabled) runs
- panel output reaches deterministic “valid renderable state”

## B. Malformed snapshot fixtures

Purpose:
- verify hard rejection of invalid authoritative payloads

Examples:
- wrong `schema_version`
- missing required field
- unknown extra property (closed object violation)
- wrong scalar type/range (e.g., negative for uint field)
- invalid enum value

Expected behavior:
- fail during snapshot schema validation (or decode gate)
- projector does **not** run
- panel remains in rejected/error input state

## C. Valid retained/model fixtures

Purpose:
- verify panel-retained/model continuity logic independent of raw snapshot parsing

Examples:
- retained expansion/sort/filter metadata with valid projection references
- continuity caches that align with projected authoritative IDs
- legal transitions from previous model → next model

Expected behavior:
- retained/model schema checks (if present) pass
- projection merge/apply passes
- panel outcome matches expected continuity behavior

## D. Malformed retained/model/projection fixtures

Purpose:
- verify model/projection gate behavior when non-authoritative panel data is invalid

Examples:
- retained state references nonexistent IDs in projection
- malformed retained structure/type mismatch
- projection artifact inconsistent with authoritative snapshot-derived model
- illegal transition metadata (e.g., stale lineage assumptions)

Expected behavior:
- fail at retained/model/projection validation stage
- authoritative snapshot may still be valid
- outcome is deterministic fallback/recovery path (or explicit reject), per case

---

## 3) Proposed per-fixture manifest

Each fixture directory includes a `manifest.json` (or `.yaml`) describing intent.

```json
{
  "fixture_id": "snapshot_malformed_missing_stream_id",
  "fixture_kind": "authoritative_snapshot",
  "expected_validity": "invalid",
  "expected_failure_stage": "snapshot_schema",
  "should_run_projector": false,
  "expected_panel_outcome": "reject_input",
  "inputs": {
    "snapshot": "snapshot.json",
    "retained": null,
    "projection": null
  },
  "notes": "Missing required field stream_id in streams[0]."
}
```

## Required manifest fields

- `fixture_id` (string)
  - stable unique ID for reporting and golden linkage.
- `fixture_kind` (enum)
  - `authoritative_snapshot`
  - `retained_model`
  - `projection_only`
  - `composite` (snapshot + retained/projection)
- `expected_validity` (enum)
  - `valid`
  - `invalid`
- `expected_failure_stage` (enum or `null`)
  - `none` (for valid fixtures)
  - `snapshot_decode`
  - `snapshot_schema`
  - `snapshot_semantic`
  - `retained_schema`
  - `projection_validation`
  - `panel_runtime`
- `should_run_projector` (boolean)
  - explicit expectation to prevent accidental stage drift.
- `expected_panel_outcome` (enum)
  - `render_ok`
  - `render_with_fallback`
  - `reject_input`
  - `error_state`
- `inputs` (object)
  - paths for `snapshot`, `retained`, `projection` (nullable where not used).

## Recommended optional fields

- `expected_error_code` (string)
- `expected_error_contains` (string/array)
- `golden_output` (path)
- `tags` (array; e.g. `schema`, `enum-edge`, `continuity`, `negative`)
- `owner` (string)
- `notes` (string)

---

## 4) Harness preflight sequence

The harness must run a deterministic preflight before executing panel assertions.

## Shared preflight (all fixtures)

1. Load manifest.
2. Validate manifest against manifest schema.
3. Resolve declared input files and hash them (for reproducibility logs).
4. Confirm fixture classification is internally consistent:
   - invalid fixtures must declare non-`none` `expected_failure_stage`
   - valid fixtures must declare `expected_failure_stage = none`

If any shared preflight step fails, classify as **harness/fixture-definition error**
(not SUT behavior).

## Preflight for fixtures expected valid

1. Decode snapshot/retained/projection inputs (as applicable).
2. Run authoritative snapshot schema validation if snapshot present.
3. Run retained/projection schema checks if applicable.
4. Run semantic gate checks required for the fixture kind.
5. Assert no failure occurred prior to expected execution stage.
6. If `should_run_projector = true`, require projector execution and success.

Any failure before panel assertion is **unexpected** and should fail the test as
accidental invalidity.

## Preflight for fixtures expected invalid

1. Decode inputs as far as needed to reach declared failure stage.
2. Execute gates in order until first failure.
3. Assert the first failure stage matches `expected_failure_stage` exactly.
4. Assert `should_run_projector` behavior is respected:
   - if false, projector must not run,
   - if true, failure must occur after projector entry.
5. Optionally assert expected error code/message fragment.

If fixture passes all gates unexpectedly, fail as **accidental validity drift**.
If it fails at a different stage, fail as **taxonomy mismatch**.

---

## 5) Distinguishing accidental invalidity vs expected-invalid data

Use explicit classification in test output:

- `EXPECTED_INVALID_CONFIRMED`
  - fixture marked invalid, failed at declared stage.
- `UNEXPECTED_INVALID`
  - fixture marked valid, but failed preflight/validation.
- `UNEXPECTED_VALID`
  - fixture marked invalid, but did not fail at declared stage.
- `INVALID_STAGE_MISMATCH`
  - fixture marked invalid, failed but at different stage.
- `HARNESS_CONFIG_ERROR`
  - manifest malformed, missing files, or contradictory manifest fields.

This classification prevents silent acceptance of broken fixtures and keeps
negative tests intentional.

---

## 6) Suggested directory layout

```text
tests/fixtures/status_panel/
  authoritative/valid/
    <fixture_id>/
      manifest.json
      snapshot.json
  authoritative/invalid/
    <fixture_id>/
      manifest.json
      snapshot.json
  retained/valid/
    <fixture_id>/
      manifest.json
      retained.json
      projection.json
      snapshot.json   # optional when composite
  retained/invalid/
    <fixture_id>/
      manifest.json
      retained.json
      projection.json
      snapshot.json   # optional when composite
```

Keep fixture payloads small and purpose-specific; use tags for cross-cutting
selection rather than duplicating large payloads.

---

## 7) Practical policy recommendations

- Require every invalid fixture to declare exactly one intended first-failure stage.
- Freeze fixture manifests under code review as strictly as test code.
- Add a CI summary table grouped by classification labels above.
- Treat manifest/schema drift as a blocking failure, not warning-only.

This keeps snapshot-contract testing, projection testing, and panel continuity
checks decoupled but composable.
