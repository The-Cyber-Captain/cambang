# Snapshot Truth Rules

Snapshot fields must always represent **real runtime truth**.

If authoritative runtime truth does not exist for a field, the snapshot
must publish `0` or an empty value rather than fabricate a placeholder.

## Acceptable states

1. Runtime truth exists and is projected.
2. Runtime truth does not yet exist and the snapshot publishes `0`.
3. Retention plumbing exists but the value remains unset until real
   truth arrives.

## Forbidden pattern

The runtime must never fabricate bootstrap values for snapshot fields.

Examples of problematic patterns:  
- ensure_camera_spec_version(hardware_id, 1)
- reset_for_generation(1)
- default_version = 1
- seed_if_missing()


A non-zero value implies that real runtime truth exists.

## Code review rule

When reviewing snapshot fields ask:

1. Where does this value become true?
2. What event authorizes that truth?
3. What happens if that event never occurs?

If the code fabricates a value in that case, the snapshot field is
incorrect.
