extends RefCounted

const CANONICAL_SCHEMA_PATH := "res://../../schema/state_snapshot/v1/state_snapshot_schema.json"

static var _cached_schema: Dictionary = {}
static var _cached_schema_load_error: String = ""


static func validate_snapshot(snapshot: Variant) -> Array[String]:
	var errors: Array[String] = []
	var schema := _load_canonical_schema(errors)
	if schema.is_empty():
		return errors
	_validate_against_schema(snapshot, schema, "$", schema, errors)
	return errors


static func _load_canonical_schema(errors: Array[String]) -> Dictionary:
	if not _cached_schema.is_empty():
		return _cached_schema
	if not _cached_schema_load_error.is_empty():
		errors.append(_cached_schema_load_error)
		return {}

	var path := ProjectSettings.globalize_path(CANONICAL_SCHEMA_PATH)
	var text := FileAccess.get_file_as_string(path)
	if text.is_empty():
		_cached_schema_load_error = "failed to read canonical snapshot schema: %s" % CANONICAL_SCHEMA_PATH
		errors.append(_cached_schema_load_error)
		return {}

	var json := JSON.new()
	var parse_err := json.parse(text)
	if parse_err != OK or typeof(json.data) != TYPE_DICTIONARY:
		_cached_schema_load_error = "failed to parse canonical snapshot schema: %s" % CANONICAL_SCHEMA_PATH
		errors.append(_cached_schema_load_error)
		return {}

	_cached_schema = json.data
	return _cached_schema


static func _validate_against_schema(
		value: Variant,
		schema_node: Dictionary,
		path: String,
		root_schema: Dictionary,
		errors: Array[String]
	) -> void:
	var effective_schema := _resolve_schema_refs(schema_node, root_schema, errors)
	if effective_schema.is_empty():
		return

	if effective_schema.has("const"):
		if value != effective_schema.get("const"):
			errors.append("%s must equal %s" % [path, str(effective_schema.get("const"))])
			return

	if effective_schema.has("enum"):
		var options: Array = effective_schema.get("enum", [])
		if not options.has(value):
			errors.append("%s must be one of %s" % [path, str(options)])
			return

	var type_name := str(effective_schema.get("type", ""))
	if not type_name.is_empty() and not _matches_type(value, type_name):
		errors.append("%s must be %s" % [path, type_name])
		return

	if type_name == "integer":
		_validate_integer_bounds(value, effective_schema, path, errors)
		return

	if type_name == "string" or type_name == "boolean":
		return

	if type_name == "array":
		var arr := value as Array
		var item_schema: Dictionary = effective_schema.get("items", {})
		for i in range(arr.size()):
			_validate_against_schema(arr[i], item_schema, "%s[%d]" % [path, i], root_schema, errors)
		return

	if type_name == "object":
		_validate_object(value, effective_schema, path, root_schema, errors)


static func _resolve_schema_refs(schema_node: Dictionary, root_schema: Dictionary, errors: Array[String]) -> Dictionary:
	if not schema_node.has("$ref"):
		return schema_node
	var ref := str(schema_node.get("$ref", ""))
	if not ref.begins_with("#/"):
		errors.append("unsupported $ref in canonical schema: %s" % ref)
		return {}
	var parts := ref.substr(2).split("/")
	var current: Variant = root_schema
	for part in parts:
		if typeof(current) != TYPE_DICTIONARY:
			errors.append("invalid $ref target in canonical schema: %s" % ref)
			return {}
		var dict_current := current as Dictionary
		if not dict_current.has(part):
			errors.append("missing $ref target in canonical schema: %s" % ref)
			return {}
		current = dict_current.get(part)
	if typeof(current) != TYPE_DICTIONARY:
		errors.append("non-object $ref target in canonical schema: %s" % ref)
		return {}
	return current as Dictionary


static func _matches_type(value: Variant, type_name: String) -> bool:
	match type_name:
		"object":
			return typeof(value) == TYPE_DICTIONARY
		"array":
			return typeof(value) == TYPE_ARRAY
		"string":
			return typeof(value) == TYPE_STRING
		"boolean":
			return typeof(value) == TYPE_BOOL
		"integer":
			return _is_json_integer(value)
		_:
			return true


static func _is_json_integer(value: Variant) -> bool:
	if typeof(value) == TYPE_INT:
		return true
	if typeof(value) == TYPE_FLOAT:
		var f := float(value)
		return is_finite(f) and floor(f) == f
	return false


static func _validate_integer_bounds(value: Variant, schema_node: Dictionary, path: String, errors: Array[String]) -> void:
	if not _is_json_integer(value):
		return
	var iv := int(value)
	if schema_node.has("minimum"):
		var min_v := int(schema_node.get("minimum"))
		if iv < min_v:
			errors.append("%s must be >= %d" % [path, min_v])
	if schema_node.has("maximum"):
		var max_v := int(schema_node.get("maximum"))
		if iv > max_v:
			errors.append("%s must be <= %d" % [path, max_v])


static func _validate_object(
		value: Variant,
		schema_node: Dictionary,
		path: String,
		root_schema: Dictionary,
		errors: Array[String]
	) -> void:
	var dict_value := value as Dictionary
	var required: Array = schema_node.get("required", [])
	for raw_key in required:
		var key := str(raw_key)
		if not dict_value.has(key):
			errors.append("%s missing required key '%s'" % [path, key])

	var props: Dictionary = schema_node.get("properties", {})
	var allow_additional := true
	if schema_node.has("additionalProperties"):
		allow_additional = bool(schema_node.get("additionalProperties"))

	for key_variant in dict_value.keys():
		var key := str(key_variant)
		if not props.has(key):
			if not allow_additional:
				errors.append("%s has unsupported key '%s'" % [path, key])
			continue
		var prop_schema: Variant = props.get(key)
		if typeof(prop_schema) != TYPE_DICTIONARY:
			continue
		_validate_against_schema(dict_value.get(key), prop_schema as Dictionary, "%s.%s" % [path, key], root_schema, errors)
