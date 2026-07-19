# ADC v2 Camera Description Contract

ADC is an external camera-description interchange standard historically
associated with Aide-De-Cam.

CamBANG is a consumer of ADC. CamBANG is also provisionally hosting and
participating in the definition of ADC v2 in this repository to establish
current compatibility, but long-term canonical ownership is intended to return
to Aide-De-Cam.

Accordingly, this repository copy is normative for the ADC v2 contract CamBANG
currently targets. It must not be read as a claim that CamBANG permanently owns
ADC as a standard.

For CamBANG-specific consumption architecture, lifecycle, matching, and result
assembly consequences, see `docs/camera_fact_model.md`. That architecture
document does not define ADC interchange semantics.

The matching machine-readable schema is:

- `schema/adc/camera_description/v2/adc_camera_description_schema.json`

ADC v1 remains historical source input only. ADC v1 permission-sentinel
conventions are not part of ADC v2.

---

## 1. Scope

ADC v2 is a human-authorable full-replacement camera-description document
format for:

- per-camera static description facts;
- optional cross-camera concurrency capability;
- optional non-authoritative document provenance.

This contract defines:

- document and record shape;
- field meanings;
- exact token vocabulary;
- units and conventions;
- strict validation of recognized members and enums;
- additive unknown-member tolerance within a supported version.

This contract does not define CamBANG runtime lifecycle, retained-state policy,
Godot API behavior, `ImagingSpec` projection, or Core admission policy.

---

## 2. Versioning

The document must carry an integer `schema_version`.

This contract defines ADC camera-description schema version `2`.

Supported-version handling rules for a consumer are:

- supported version: parse recognized members, validate recognized semantics,
  ignore unknown additive members if otherwise valid;
- unsupported version: reject the document;
- malformed version field: reject the document.

Version support policy for a particular consumer, including CamBANG, is a
consumer-specific compatibility matter rather than an ADC-standard semantic.

---

## 3. Top-Level Shape

The top-level document is an object with:

- required `schema_version`;
- required `cameras`;
- optional `concurrent_camera_support`;
- optional document provenance fields.

`cameras` is the full replacement set of camera entries described by the
document.

`cameras: []` is valid and represents an explicit empty full-replacement camera
set.

Omission of the required `cameras` member is invalid.

### 3.1 Document provenance

Optional non-authoritative provenance fields may include:

- `generator`
- `generator_version`
- `timestamp_ms`
- `device_model`
- `device_manufacturer`
- `android_version`
- `godot_version`

These fields describe document provenance only. They are not camera facts.

---

## 4. Camera Identity

Each camera entry must contain:

- `camera_id`

`camera_id` is an opaque, exact, case-sensitive string identifier.

Rules:

- duplicate `camera_id` values are invalid;
- consumers must not normalize, case-fold, trim, prefix, reinterpret
  numerically, or otherwise heuristically rewrite `camera_id`;
- a camera entry may be valid even if a particular consumer cannot currently
  match it to a local camera inventory.

---

## 5. Fact Source Vocabulary

Static fact records use the source vocabulary:

```text
native_reported
user_supplied
derived
virtual_camera_authored
unknown
```

This vocabulary applies to classification records and to atomic records such as
intrinsics, distortion, and pose.

---

## 6. Camera Classification

Each camera entry may include:

- `facing`
- `camera_nature`
- `sensor_orientation`

Each classification fact is a small sourced record. The old bare scalar forms
are not valid in ADC v2.

### 6.1 Facing

`facing` record fields:

- required `source`
- required `value`

`value` enum:

```text
front
back
external
unknown
```

### 6.2 Camera nature

`camera_nature` record fields:

- required `source`
- required `value`

`value` enum:

```text
physical
virtual
hybrid
unknown
```

### 6.3 Sensor orientation

`sensor_orientation` record fields:

- required `source`
- required `value_degrees`

`value_degrees` enum:

```text
0
90
180
270
```

Rules:

- each classification record is independently optional;
- when present, both `source` and the value field are required;
- absence means unavailable;
- do not introduce sibling fields such as `facing_source`;
- do not infer fact source from document-level provenance.

---

## 7. Concurrency Capability

`concurrent_camera_support` is an optional cross-camera capability record.

Shape:

- required `supported`
- optional `camera_id_combinations`

### 7.1 `supported = false`

Accepted forms:

- `camera_id_combinations` absent
- `camera_id_combinations: []`

Rejected contradictory form:

- `supported: false` with any non-empty combination list

### 7.2 `supported = true`

`camera_id_combinations` must be present as a non-empty array of camera-ID
arrays.

Each combination describes one supported concurrent camera-ID combination.

Within one combination:

- duplicate member IDs are invalid;
- singleton combinations are invalid;
- member IDs must be valid strings.

Combination members refer to camera IDs, not provider instances, runtime object
IDs, or transport-specific handles.

---

## 8. Atomic Intrinsics Record

`intrinsics` is an atomic record. It replaces or is absent as a whole.

Fields:

- required `source`
- required `focal_length_x_px`
- required `focal_length_y_px`
- required `principal_point_x_px`
- required `principal_point_y_px`
- optional `skew_px`
- required `reference_width_px`
- required `reference_height_px`
- required `coordinate_domain`
- optional `platform_defined_domain`

Units and rules:

- focal lengths, principal point, and skew are in pixels;
- reference dimensions are positive integer pixels;
- `coordinate_domain` is required;
- missing `skew_px` means unavailable, not zero;
- finite numeric values are required;
- `platform_defined_domain` is required when `coordinate_domain` is
  `platform_defined`;
- `platform_defined_domain` is forbidden for other coordinate domains.

Coordinate domains:

```text
android_sensor_pre_correction_active_array
android_sensor_active_array
delivered_image
platform_defined
```

---

## 9. Atomic Distortion Record

`distortion` is an atomic record.

Supported models:

```text
brown_conrady_5
none
```

Fields for `brown_conrady_5`:

- required `source`
- required `model`
- required `radial_k1`
- required `radial_k2`
- required `radial_k3`
- required `tangential_p1`
- required `tangential_p2`
- required `reference_width_px`
- required `reference_height_px`
- required `coordinate_domain`
- required `image_state`
- optional `platform_defined_domain`

Fields for `none`:

- required `source`
- required `model`
- required `image_state`

Image-state enums:

```text
distorted
rectified
unknown
```

Rules:

- all coefficients must be finite numbers;
- `platform_defined_domain` is required when a Brown-Conrady record uses
  `coordinate_domain: platform_defined`;
- `platform_defined_domain` is forbidden for other Brown-Conrady coordinate
  domains;
- `model: none` is the explicit no-distortion case;
- when `model: none`, Brown-Conrady-specific recognized fields are forbidden:
  `radial_k1`, `radial_k2`, `radial_k3`, `tangential_p1`, `tangential_p2`,
  `reference_width_px`, `reference_height_px`, `coordinate_domain`, and
  `platform_defined_domain`.

---

## 10. Atomic Pose Record

`pose` is an atomic per-camera record.

Fields:

- required `source`
- required `reference_kind`
- required `coordinate_convention`
- required `translation_m`
- required `rotation_xyzw`
- optional `reference_camera_id`
- optional `reference_id`
- optional `platform_defined_reference`
- optional `platform_defined_convention`

Reference-kind enums:

```text
camera
primary_camera
device_motion_sensor
automotive_reference
custom_reference
platform_defined
unknown
```

Coordinate-convention enums:

```text
android_camera2
camera_optical_frame
platform_defined
```

Rules:

- translation is three finite numbers in metres;
- quaternion order is `x, y, z, w`;
- quaternion values must be finite and not all zero;
- `reference_kind: camera` requires `reference_camera_id` and forbids
  `reference_id` and `platform_defined_reference`;
- `reference_kind: custom_reference` requires `reference_id` and forbids
  `reference_camera_id` and `platform_defined_reference`;
- `reference_kind: platform_defined` requires `platform_defined_reference` and
  forbids `reference_camera_id` and `reference_id`;
- `reference_kind: primary_camera`, `device_motion_sensor`,
  `automotive_reference`, and `unknown` forbid `reference_camera_id`,
  `reference_id`, and `platform_defined_reference`;
- `coordinate_convention: platform_defined` requires
  `platform_defined_convention`;
- other coordinate conventions forbid `platform_defined_convention`;
- a camera cannot reference itself via `reference_camera_id`.

`camera_optical_frame` means:

```text
+X = image right
+Y = image down
+Z = forward along the optical axis
```

For that convention, translation locates the subject-camera origin in the
reference frame and the quaternion rotates vectors from subject-camera
coordinates into reference-frame coordinates.

---

## 11. Absence Semantics

ADC v2 uses omission for absence.

Examples:

- omitted `facing` means unavailable facing;
- omitted `intrinsics` means no intrinsics record is provided;
- omitted `skew_px` means unavailable skew value;
- omitted `concurrent_camera_support` means no concurrency capability record is
  supplied by the document.

ADC v1 permission-sentinel conventions such as
`CAMERA_PERMISSION_REQUIRED` are not part of ADC v2.

---

## 12. Strict Validation and Additive Tolerance

Within a supported schema version:

- recognized fields must have the correct type;
- recognized enum fields must use one of the defined tokens exactly;
- required fields for a recognized record must be present;
- finite-number requirements apply to numeric calibration and pose fields;
- cross-field dependency rules must be satisfied;
- recognized fields that are forbidden for a selected variant must cause
  rejection.

At the same time, additive unknown-member tolerance is allowed within a
supported version:

- unknown top-level members may be ignored;
- unknown members inside recognized objects may be ignored;
- unknown members must not change the meaning of recognized members.

This supports additive schema growth without relaxing validation of the
recognized contract.

---

## 13. Human-Authorable Examples

### 13.1 Populated example

```json
{
  "schema_version": 2,
  "generator": "Aide-De-Cam",
  "timestamp_ms": 1780000000000,
  "cameras": [
    {
      "camera_id": "0",
      "facing": {
        "source": "native_reported",
        "value": "back"
      },
      "camera_nature": {
        "source": "native_reported",
        "value": "physical"
      },
      "sensor_orientation": {
        "source": "native_reported",
        "value_degrees": 90
      },
      "intrinsics": {
        "source": "native_reported",
        "focal_length_x_px": 3120.4,
        "focal_length_y_px": 3118.9,
        "principal_point_x_px": 2014.3,
        "principal_point_y_px": 1508.7,
        "reference_width_px": 4032,
        "reference_height_px": 3024,
        "coordinate_domain": "android_sensor_pre_correction_active_array"
      },
      "distortion": {
        "source": "native_reported",
        "model": "brown_conrady_5",
        "radial_k1": 0.01,
        "radial_k2": -0.02,
        "radial_k3": 0.003,
        "tangential_p1": 0.0002,
        "tangential_p2": -0.0001,
        "reference_width_px": 4032,
        "reference_height_px": 3024,
        "coordinate_domain": "android_sensor_pre_correction_active_array",
        "image_state": "distorted"
      }
    },
    {
      "camera_id": "synthetic:0",
      "camera_nature": {
        "source": "virtual_camera_authored",
        "value": "virtual"
      },
      "sensor_orientation": {
        "source": "virtual_camera_authored",
        "value_degrees": 0
      },
      "distortion": {
        "source": "virtual_camera_authored",
        "model": "none",
        "image_state": "rectified"
      },
      "pose": {
        "source": "virtual_camera_authored",
        "reference_kind": "custom_reference",
        "reference_id": "synthetic_rig",
        "coordinate_convention": "camera_optical_frame",
        "translation_m": [0.0, 0.0, 0.0],
        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0]
      }
    }
  ],
  "concurrent_camera_support": {
    "supported": true,
    "camera_id_combinations": [
      ["0", "1"]
    ]
  }
}
```

### 13.2 Explicit empty full replacement

```json
{
  "schema_version": 2,
  "cameras": []
}
```
