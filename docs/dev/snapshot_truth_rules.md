# Snapshot Truth Rules

Status: Canonical Dev Note  
Supplements: `state_snapshot.md`

This document is the canonical definition of snapshot truth requirements
for providers and maintainers.

All other documentation that references snapshot truth should link to
this document rather than restating the rules.

## Principle

Snapshot fields must represent **actual retained runtime truth**.

They must never contain:

- fabricated bootstrap values
- guessed provider state
- predicted future state
- synthetic repair values introduced only to make snapshots look complete

If authoritative runtime truth does not exist for a field, the snapshot
must publish `0`, `NIL`, or an empty value as appropriate rather than
fabricate a placeholder.

## Acceptable States

A snapshot field is valid only when one of the following is true:

1. runtime truth exists and is projected faithfully
2. runtime truth does not yet exist and the snapshot publishes the
   correct empty or zero value
3. retention plumbing exists but the field remains unset until real truth
   arrives

A non-zero or non-empty value implies that real runtime truth exists.

## When Zero, Empty, or `NIL` Is Correct

Fields may legitimately remain zero, empty, or `NIL` when:

- the runtime has not yet observed the value
- the provider has not yet reported the information
- the resource does not exist
- a new generation has started and no authoritative publication has yet
  established replacement truth

Unknown values must remain unknown until authorized by runtime state.

## Core Rules

### Truthful presence

A field may appear populated only when the runtime actually retains the
corresponding truth.

### Truthful absence

If the runtime does not retain that truth, the snapshot must expose the
correct empty representation rather than a guessed stand-in.

### No synthetic repair

The runtime must not backfill missing snapshot fields with convenience
values, bootstrap seeds, inferred identifiers, or default versions.

### Stable identity

Identifiers exposed in snapshots must come from the retained runtime
identity model. They must not be invented to make publications appear
stable.

### Snapshot consistency

Each published snapshot must be internally consistent with the retained
runtime state that authorized the publication.

## Applied Configuration Rule

Applied capture profile of a live runtime entity is part of runtime truth.
The snapshot must expose a faithful, behaviourally complete projection of that applied profile.
`profile_version` is a monotonic signal of change and must not be used as a substitute for the profile itself.

- The applied capture profile of any live runtime entity is part of runtime truth.
- Snapshot must expose a faithful, behaviourally complete projection of that applied profile.
- `profile_version` is auxiliary metadata indicating that the applied profile changed.
- `profile_version` must not be used to infer configuration contents.
- Snapshot must not omit any applied profile field that affects observable runtime behaviour.
- Snapshot must not fabricate or infer profile values.

## Invalid Patterns

Examples of invalid snapshot behaviour include:

- populating format fields before the provider reports them
- inventing topology identifiers
- guessing warm retention values
- seeding default versions before the corresponding truth exists
- repairing absent state by publishing a synthetic non-zero value

Examples of problematic code patterns:

- `ensure_camera_spec_version(hardware_id, 1)`
- `reset_for_generation(1)`
- `default_version = 1`
- `seed_if_missing()`

## Code Review Rule

When reviewing snapshot fields, ask:

1. where does this value become true?
2. what event authorizes that truth?
3. what happens if that event never occurs?

If the code fabricates a value in that case, the snapshot field is
incorrect.

## Adding New Snapshot Fields

When adding fields:

1. the value must originate from retained runtime state
2. the snapshot must remain immutable after publication
3. unknown values must remain unknown instead of fabricated
4. empty or zero values must be chosen because they truthfully represent
   absence, not because they are convenient defaults

## Where These Rules Are Enforced

These rules are exercised or assumed by:

- `docs/dev/maintainer_tools.md`
- `docs/architecture/publication_model.md`
- `docs/state_snapshot.md`
