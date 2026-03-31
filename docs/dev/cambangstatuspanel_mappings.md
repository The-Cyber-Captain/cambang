## CamBANGStatusPanel: implemented health rules and lifecycle badge mapping

This documents the rules currently applied by the packaged `CamBANGStatusPanel` code.

### Health summary labels

The panel inserts a single leading health badge per row using these labels:

- `OK`
- `ATTN`
- `BAD`
- `UNKNOWN`

Health badge role mapping is:

- `OK` -> `success`
- `ATTN` -> `warning`
- `BAD` -> `error`
- `UNKNOWN` -> `neutral`

Health is applied ahead of all other badges.

---

## Lifecycle badge phase parsing

Lifecycle-role remapping uses a common phase parser that recognizes all of:

- `phase=CREATED|LIVE|TEARING_DOWN|DESTROYED`
- `native_phase=CREATED|LIVE|TEARING_DOWN|DESTROYED`
- raw badge labels `CREATED`, `LIVE`, `TEARING_DOWN`, `DESTROYED`
- special lowercase `destroyed` -> `DESTROYED`

If a badge does not parse as one of those lifecycle phases, no lifecycle-role remap is applied.

---

## Lifecycle badge role mapping

### 1) Native rows (`is_native_record == true`)

#### 1a) Native rows in below-line or orphan-branch context
Applied when either of these is true:

- `is_below_line`
- `is_in_orphan_native_branch`

Mapping:

- `CREATED` -> `warning`
- `LIVE` -> `error`
- `TEARING_DOWN` -> `warning`
- `DESTROYED` -> `success`

#### 1b) Native rows not below-line and not in orphan branch

Mapping:

- `CREATED` -> `info`
- `LIVE` -> `success`
- `TEARING_DOWN` -> `warning`
- `DESTROYED` -> `neutral`

---

### 2) Provider rows in orphan context

Applied when:

- `visual_object_class == "provider"`
- provider has badge label `orphaned`

Mapping:

- `CREATED` -> `warning`
- `LIVE` -> `error`
- `TEARING_DOWN` -> `warning`
- `DESTROYED` -> `success`

---

### 3) Generic non-native rows

Rows are classified as either:

- `retained` if `_is_retained_projection_entry(entry.id)` is true
- otherwise `authoritative`

#### 3a) Retained rows

- `CREATED` -> `warning`
- `LIVE` -> `success`
- `TEARING_DOWN` -> `warning`
- `DESTROYED` -> `success`

#### 3b) Authoritative rows

- `CREATED` -> `info`
- `LIVE` -> `success`
- `TEARING_DOWN` -> `warning`
- `DESTROYED` -> `neutral`

---

## Server health rules

Server health facts include:

- contract/projection failure
- `NO SNAPSHOT`
- server native coverage state
- server version delta against the previous observed server state
- topology growth rate over the observation window

### Server = `BAD` if any of:
- contract or projection failure exists
- native coverage state is `MISSING`
- `version_delta > 1`

### Server = `UNKNOWN` if any of:
- server has `NO SNAPSHOT`
- native coverage state is `UNKNOWN`

### Server = `ATTN` if:
- topology growth rate exceeds `server_topology_growth_rate_attn_threshold_per_sec`
- if that threshold is `0`, this rule is disabled

### Otherwise:
- server = `OK`

Priority order:

- `BAD`
- `UNKNOWN`
- `ATTN`
- `OK`

---

## Provider health rules

Provider health facts include:

- contract/projection failure
- provider aggregate counter consistency:
    - `native_all`
    - `native_cur`
    - `native_prev`
    - `native_dead`
- lifecycle contradiction
- insufficient local truth
- preserved state
- destroyed state
- orphaned state
- provider phase

A provider is considered:

- **preserved** if it is a retained projection entry or has badge `continuity-only`
- **destroyed** if it has badge `destroyed` or its parsed provider phase is `destroyed`
- **orphaned** if it has badge `orphaned`

### Provider counter inconsistency (`BAD`) if any of:
- any of `native_all`, `native_cur`, `native_prev`, `native_dead` is negative
- `native_cur > native_all`
- `native_prev > native_all`
- `native_dead > native_all`
- `(native_cur + native_prev) > native_all`

### Provider lifecycle contradiction (`BAD`) if:
- parsed provider phase says destroyed
- but semantic destroyed test disagrees

### Provider insufficient local truth (`UNKNOWN`) if:
- provider phase cannot be parsed from badges

### Provider = `BAD` if any of:
- contract/projection failure
- counter inconsistency
- lifecycle contradiction
- provider is orphaned and phase is `LIVE`

### Provider = `UNKNOWN` if:
- insufficient local truth

### Provider = `ATTN` if any of:
- provider is orphaned and phase is `CREATED`
- provider is orphaned and phase is `TEARING_DOWN`
- provider is preserved and not destroyed

### Otherwise:
- provider = `OK`

Priority order:

- `BAD`
- `UNKNOWN`
- `ATTN`
- `OK`

---

## Device health rules

Device health facts include:

- contract/projection failure
- counter inconsistency
- lifecycle contradiction
- insufficient local truth
- preserved state
- destroyed state
- current `errors` counter
- device error growth rate over the observation window

A device is considered:

- **preserved** if it is a retained projection entry or has badge `continuity-only`
- **destroyed** if it has badge `destroyed` or its parsed phase is `destroyed`

### Device counter inconsistency
Current implementation only checks:

- `errors < 0`

This check is disabled for preserved rows.

### Device lifecycle contradiction (`BAD`) if:
- parsed phase says destroyed
- but semantic destroyed test disagrees

### Device insufficient local truth (`UNKNOWN`) if:
- row is not preserved
- and `errors < 0`

### Device = `BAD` if any of:
- contract/projection failure
- counter inconsistency
- lifecycle contradiction
- error growth rate exceeds `device_error_growth_rate_bad_threshold_per_sec`
- if that threshold is `0`, the temporal growth rule is disabled

### Device = `UNKNOWN` if:
- insufficient local truth

### Device = `ATTN` if any of:
- device is preserved and not destroyed
- current `errors > 0`

### Otherwise:
- device = `OK`

Priority order:

- `BAD`
- `UNKNOWN`
- `ATTN`
- `OK`

---

## Stream health rules

Stream health facts include:

- contract/projection failure
- lifecycle contradiction
- insufficient local truth
- preserved state
- destroyed state
- parsed `mode`
- parsed `stop_reason`
- counters:
    - `drop`
    - `rej_fmt`
    - `rej_inv`
- growth rates over the observation window for:
    - `drop`
    - `rej_fmt`
    - `rej_inv`

A stream is considered:

- **preserved** if it is a retained projection entry or has badge `continuity-only`
- **destroyed** if it has badge `destroyed` or its parsed phase is `destroyed`

### Stream lifecycle contradiction (`BAD`) if:
- parsed phase says destroyed
- but semantic destroyed test disagrees

### Stream insufficient local truth (`UNKNOWN`) if:
- row is not preserved
- row is not destroyed
- parsed mode is empty

### Stream = `BAD` if any of:
- contract/projection failure
- lifecycle contradiction
- mode is `ERROR`
- mode is `FLOWING` or `STARVED` and drop growth rate exceeds `stream_drop_growth_rate_bad_threshold_per_sec`
- mode is `FLOWING` or `STARVED` and reject-format growth rate exceeds `stream_rej_fmt_growth_rate_bad_threshold_per_sec`
- mode is `FLOWING` or `STARVED` and reject-invalid growth rate exceeds `stream_rej_inv_growth_rate_bad_threshold_per_sec`

If any stream growth threshold is `0`, that specific temporal rule is disabled.

### Stream = `UNKNOWN` if:
- insufficient local truth

### Special-case preserved destroyed stream
If a stream is both:

- preserved
- destroyed

then it is forced to `OK` before `ATTN` checks.

### Stream = `ATTN` if any of:
- preserved and not destroyed
- mode is `STARVED`
- mode is `STOPPED` and stop reason is `PREEMPTED`
- mode is `STOPPED` and stop reason is `PROVIDER`
- mode is `FLOWING` or `STARVED` and current `drop > 0`
- mode is `FLOWING` or `STARVED` and current `rej_fmt > 0`
- mode is `FLOWING` or `STARVED` and current `rej_inv > 0`

### Otherwise:
- stream = `OK`

Priority order:

- `BAD`
- `UNKNOWN`
- special preserved-destroyed override -> `OK`
- `ATTN`
- `OK`

---

## Native-row health rules

Native health facts include:

- contract/projection failure
- below-line flag
- orphan-branch flag
- parsed native phase

Important implementation detail:

- native health uses `is_in_orphan_native_branch`
- not merely row-local orphan-root truth

### Native = `BAD` if any of:
- contract/projection failure
- row is below-line or in orphan branch, and phase is `LIVE`

### Native = `UNKNOWN` if:
- no contract/projection failure
- phase is missing

Also:

- if row is **not** below-line
- and **not** in orphan branch
- and phase is `DESTROYED`

then native health is `UNKNOWN`

### Native = `ATTN` if:
- row is below-line or in orphan branch, and phase is `CREATED`
- row is below-line or in orphan branch, and phase is `TEARING_DOWN`
- row is neither below-line nor orphan-branch, and phase is `TEARING_DOWN`

### Otherwise:
- native = `OK`

Priority order:

- `BAD`
- `UNKNOWN`
- `ATTN`
- `OK`

---

## Contract / projection failure detection

For server, provider, device, stream, and native rows, contract/projection failure is treated as a high-priority negative signal.

Depending on row type, this is detected from some combination of:

- badge `contract-gap`
- badge `CONTRACT GAP`
- badge `snapshot-incompatible`
- existence of `<entry.id>/contract_gaps`
- for server also `server/main/projection_gaps`
- anomaly info lines

Where present, these failures force `BAD` for:

- server
- provider
- device
- stream
- native rows

---

## Preservation / retention semantics used by health

### Provider preserved
True if:

- retained projection entry, or
- badge `continuity-only`

### Device preserved
True if:

- retained projection entry, or
- badge `continuity-only`

### Stream preserved
True if:

- retained projection entry, or
- badge `continuity-only`

### Native preserved
Current helper treats native rows as preserved if:

- `is_below_line`, or
- badge `continuity-only`

Note: native preserved state is not currently the primary health driver; native health is mostly controlled by:
- contract/projection failure
- below-line status
- orphan-branch status
- phase

---

## Orphan semantics used by health / lifecycle

### Provider orphaned
True if provider row has badge:

- `orphaned`

### Native orphaned
True for health/lifecycle purposes if:

- `is_in_orphan_native_branch == true`

That means the implemented native orphan policy applies to:
- the orphan root itself
- descendants within the orphan native branch

This is distinct from row-local orphan-root truth.

---

## Temporal observation basis

Temporal health rules operate on the panel’s observed per-row history, using Godot-visible panel updates.

Observed-history series are maintained for:

- server
- device rows
- stream rows

Current temporal checks are:

- server topology growth rate
- device error growth rate
- stream growth rates for `drop`, `rej_fmt`, `rej_inv`

Elapsed time prefers snapshot `timestamp_ns`; if unavailable, it falls back to observed wall-clock milliseconds.

A threshold value of `0` disables that specific temporal growth rule.

---