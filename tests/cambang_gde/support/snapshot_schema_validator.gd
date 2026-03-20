extends RefCounted

const VALID_PHASES := {
	"CREATED": true,
	"LIVE": true,
	"TEARING_DOWN": true,
	"DESTROYED": true,
}

const VALID_RIG_MODES := {
	"OFF": true,
	"ARMED": true,
	"TRIGGERING": true,
	"COLLECTING": true,
	"ERROR": true,
}

const VALID_DEVICE_MODES := {
	"IDLE": true,
	"STREAMING": true,
	"CAPTURING": true,
	"ERROR": true,
}

const VALID_STREAM_MODES := {
	"STOPPED": true,
	"FLOWING": true,
	"STARVED": true,
	"ERROR": true,
}

const VALID_STREAM_INTENTS := {
	"PREVIEW": true,
	"VIEWFINDER": true,
}

const VALID_STREAM_STOP_REASONS := {
	"NONE": true,
	"USER": true,
	"PREEMPTED": true,
	"PROVIDER": true,
}

const VALID_NATIVE_OBJECT_TYPES := {
	"provider": true,
	"device": true,
	"stream": true,
	"frameproducer": true,
}

const TOP_LEVEL_KEYS := {
	"schema_version": true,
	"gen": true,
	"version": true,
	"topology_version": true,
	"timestamp_ns": true,
	"imaging_spec_version": true,
	"rigs": true,
	"devices": true,
	"streams": true,
	"native_objects": true,
	"detached_root_ids": true,
}

const RIG_KEYS := {
	"rig_id": true,
	"phase": true,
	"mode": true,
	"devices": true,
}

const DEVICE_KEYS := {
	"instance_id": true,
	"hardware_id": true,
	"phase": true,
	"mode": true,
	"errors_count": true,
}

const STREAM_KEYS := {
	"stream_id": true,
	"device_instance_id": true,
	"phase": true,
	"mode": true,
	"intent": true,
	"stop_reason": true,
	"target_fps_max": true,
	"frames_received": true,
}

const NATIVE_OBJECT_KEYS := {
	"native_id": true,
	"type": true,
	"phase": true,
	"root_id": true,
	"creation_gen": true,
	"created_ns": true,
	"destroyed_ns": true,
	"bytes_allocated": true,
	"buffers_in_use": true,
	"owner_provider_native_id": true,
	"owner_rig_id": true,
	"owner_device_instance_id": true,
	"owner_stream_id": true,
}


static func validate_snapshot(snapshot: Variant) -> Array[String]:
	var errors: Array[String] = []

	if typeof(snapshot) != TYPE_DICTIONARY:
		errors.append("snapshot root must be Dictionary")
		return errors

	_validate_closed_object(snapshot, TOP_LEVEL_KEYS, "snapshot", errors)

	_require_int(snapshot, "schema_version", "snapshot", errors)
	_require_int(snapshot, "gen", "snapshot", errors)
	_require_int(snapshot, "version", "snapshot", errors)
	_require_int(snapshot, "topology_version", "snapshot", errors)
	_require_int(snapshot, "timestamp_ns", "snapshot", errors)
	_require_int(snapshot, "imaging_spec_version", "snapshot", errors)
	_require_array(snapshot, "rigs", "snapshot", errors)
	_require_array(snapshot, "devices", "snapshot", errors)
	_require_array(snapshot, "streams", "snapshot", errors)
	_require_array(snapshot, "native_objects", "snapshot", errors)
	_require_array(snapshot, "detached_root_ids", "snapshot", errors)

	if snapshot.has("schema_version"):
		if not _is_json_int_like(snapshot["schema_version"]) or int(round(float(snapshot["schema_version"]))) != 1:
			errors.append("snapshot.schema_version must equal 1")

	var rigs: Array = snapshot.get("rigs", [])
	for i in range(rigs.size()):
		_validate_rig(rigs[i], "snapshot.rigs[%d]" % i, errors)

	var devices: Array = snapshot.get("devices", [])
	for i in range(devices.size()):
		_validate_device(devices[i], "snapshot.devices[%d]" % i, errors)

	var streams: Array = snapshot.get("streams", [])
	for i in range(streams.size()):
		_validate_stream(streams[i], "snapshot.streams[%d]" % i, errors)

	var native_objects: Array = snapshot.get("native_objects", [])
	for i in range(native_objects.size()):
		_validate_native_object(native_objects[i], "snapshot.native_objects[%d]" % i, errors)

	var detached_root_ids: Array = snapshot.get("detached_root_ids", [])
	for i in range(detached_root_ids.size()):
		if not _is_json_int_like(detached_root_ids[i]):
			errors.append("snapshot.detached_root_ids[%d] must be int" % i)

	return errors


static func _validate_rig(rig: Variant, path: String, errors: Array[String]) -> void:
	if typeof(rig) != TYPE_DICTIONARY:
		errors.append("%s must be Dictionary" % path)
		return

	_validate_closed_object(rig, RIG_KEYS, path, errors)
	_require_int(rig, "rig_id", path, errors)
	_require_enum(rig, "phase", VALID_PHASES, path, errors)
	_require_enum(rig, "mode", VALID_RIG_MODES, path, errors)
	_require_array(rig, "devices", path, errors)

	var devices: Array = rig.get("devices", [])
	for i in range(devices.size()):
		if not _is_json_int_like(devices[i]):
			errors.append("%s.devices[%d] must be int" % [path, i])


static func _validate_device(device: Variant, path: String, errors: Array[String]) -> void:
	if typeof(device) != TYPE_DICTIONARY:
		errors.append("%s must be Dictionary" % path)
		return

	_validate_closed_object(device, DEVICE_KEYS, path, errors)
	_require_int(device, "instance_id", path, errors)
	_require_string(device, "hardware_id", path, errors)
	_require_enum(device, "phase", VALID_PHASES, path, errors)
	_require_enum(device, "mode", VALID_DEVICE_MODES, path, errors)
	_require_int(device, "errors_count", path, errors)


static func _validate_stream(stream: Variant, path: String, errors: Array[String]) -> void:
	if typeof(stream) != TYPE_DICTIONARY:
		errors.append("%s must be Dictionary" % path)
		return

	_validate_closed_object(stream, STREAM_KEYS, path, errors)
	_require_int(stream, "stream_id", path, errors)
	_require_int(stream, "device_instance_id", path, errors)
	_require_enum(stream, "phase", VALID_PHASES, path, errors)
	_require_enum(stream, "mode", VALID_STREAM_MODES, path, errors)
	_require_enum(stream, "intent", VALID_STREAM_INTENTS, path, errors)
	_require_enum(stream, "stop_reason", VALID_STREAM_STOP_REASONS, path, errors)
	_require_int(stream, "target_fps_max", path, errors)
	_require_int(stream, "frames_received", path, errors)


static func _validate_native_object(obj: Variant, path: String, errors: Array[String]) -> void:
	if typeof(obj) != TYPE_DICTIONARY:
		errors.append("%s must be Dictionary" % path)
		return

	_validate_closed_object(obj, NATIVE_OBJECT_KEYS, path, errors)
	_require_int(obj, "native_id", path, errors)
	_require_enum(obj, "type", VALID_NATIVE_OBJECT_TYPES, path, errors)
	_require_enum(obj, "phase", VALID_PHASES, path, errors)
	_require_int(obj, "creation_gen", path, errors)

	_optional_int(obj, "root_id", path, errors)
	_optional_int(obj, "created_ns", path, errors)
	_optional_int(obj, "destroyed_ns", path, errors)
	_optional_int(obj, "bytes_allocated", path, errors)
	_optional_int(obj, "buffers_in_use", path, errors)
	_optional_int(obj, "owner_provider_native_id", path, errors)
	_optional_int(obj, "owner_rig_id", path, errors)
	_optional_int(obj, "owner_device_instance_id", path, errors)
	_optional_int(obj, "owner_stream_id", path, errors)


static func _validate_closed_object(obj: Dictionary, allowed: Dictionary, path: String, errors: Array[String]) -> void:
	for key in obj.keys():
		var key_s := str(key)
		if not allowed.has(key_s):
			errors.append("%s has unknown field '%s'" % [path, key_s])


static func _require_int(obj: Dictionary, key: String, path: String, errors: Array[String]) -> void:
	if not obj.has(key):
		errors.append("%s missing '%s'" % [path, key])
		return

	if not _is_json_int_like(obj[key]):
		errors.append("%s.%s must be int" % [path, key])


static func _optional_int(obj: Dictionary, key: String, path: String, errors: Array[String]) -> void:
	if not obj.has(key):
		return

	if not _is_json_int_like(obj[key]):
		errors.append("%s.%s must be int when present" % [path, key])


static func _require_string(obj: Dictionary, key: String, path: String, errors: Array[String]) -> void:
	if not obj.has(key):
		errors.append("%s missing '%s'" % [path, key])
		return

	if typeof(obj[key]) != TYPE_STRING:
		errors.append("%s.%s must be string" % [path, key])


static func _require_array(obj: Dictionary, key: String, path: String, errors: Array[String]) -> void:
	if not obj.has(key):
		errors.append("%s missing '%s'" % [path, key])
		return

	if typeof(obj[key]) != TYPE_ARRAY:
		errors.append("%s.%s must be array" % [path, key])


static func _require_enum(obj: Dictionary, key: String, valid: Dictionary, path: String, errors: Array[String]) -> void:
	if not obj.has(key):
		errors.append("%s missing '%s'" % [path, key])
		return

	var value: Variant = obj[key]

	if typeof(value) != TYPE_STRING:
		errors.append("%s.%s must be string enum" % [path, key])
		return

	if not valid.has(value):
		errors.append("%s.%s invalid '%s'" % [path, key, value])

static func _is_json_int_like(value: Variant) -> bool:
	if typeof(value) == TYPE_INT:
		return true

	if typeof(value) == TYPE_FLOAT:
		var f: float = value
		return is_equal_approx(f, round(f))

	return false
