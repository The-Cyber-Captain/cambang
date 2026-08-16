# Current tranche

## The usable surface: trigger identity, completion signals, canonical wrappers

Tranche 5 of `capture_identity_and_lifecycle.md`. **This is the public-surface
break**, agreed with the maintainer as such.

Four tranches have landed internal correctness with almost nothing visible to a
caller: `trigger_capture()` still returns `Error`, there are no completion
signals, and `get_result()` still cannot distinguish a partial set from a final
one. This tranche is the part that makes the model usable, and the point at
which harnesses can delete their hand-rolled completion detection (§8).

### Scope

1. `CamBANGDevice.trigger_capture()` returns `{ id, error }`;
   `CamBANGRig.trigger_capture()` returns `{ id, members, error }` where
   `members` maps hardware id to the member's Device Capture Id (§4.1). Keys
   always present.
2. Completion signals (§4.2): per-object on `CamBANGDevice` and `CamBANGRig`,
   and server-wide on `CamBANGServer` carrying the settled id.
3. Canonical wrappers: `get_rig(...)` and `get_device_for_hardware_id(...)`
   return the same instance for the same id. Without this, per-object signals
   are unreliable by construction.
4. `CamBANGServer.create_rig(...)` takes `CamBANGDevice` handles, matching
   `add_member`/`remove_member`. Same break, same consumers, one migration.
5. Migrate the consumers this breaks: 12 scripts and `999_rig_result_completeness_probe.tscn`.
   `01_basic_ux.tscn` and `1001_basic_quest_snap.tscn` need no change (untyped,
   printed only); `100_basic_ux_demo.tscn`'s call is commented out.
6. Update `capture_identity_and_lifecycle.md` §0 and §9 in the same commit.

### Out of scope

- **Durable `dc_`/`rc_` public ids (§2.2).** Ids stay session-scoped integers.
  The API shape does not change again when they gain durability later.
- §2.3 result fields (`capture_origin`, `rig_member_index`, …).
- §4.5's server-exposed outstanding set. Scope items 1 and 2 already let a
  caller track its own outstanding work, which is what §4.5 says is
  "deliberately sufficient on its own".
- `get_capture_result_by_id` / `get_capture_result_set_by_id` keying. They stay
  integer-keyed advanced/diagnostic surface.
